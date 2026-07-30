# Architecture

## The idea

To compare two copies of a file, you normally have to read both copies.

That is an expensive sentence when one copy lives in S3. You pay for the egress. You wait for the
download. And you learn nothing you could not have learned from a checksum.

So `mito` never downloads the big ones. It gets S3 to do the checksumming.

## The trick

S3 supports a feature called **additional checksums**. Here is the useful consequence.

If you ask S3 to copy a byte range of an existing object (an `UploadPartCopy`), S3 reads those
bytes on its own hardware, computes a CRC32 over them, and returns the value to you in a response
header called `x-amz-checksum-crc32`.

That is a checksum of the remote bytes. And the bytes never crossed the network.

So the S3 side of a comparison works like this:

```
  1.  CreateMultipartUpload  on a scratch key
      └─► gives you an upload ID to hang the requests off

  2.  UploadPartCopy  × one per 8 MiB chunk, all in parallel
      each one naming a byte range of the object you care about
      └─► each response carries x-amz-checksum-crc32

  3.  decode that header from Base64 into a uint32_t

  4.  AbortMultipartUpload  on the scratch upload
      always. including every error path.
```

A 200 GB object becomes roughly 25,000 small API calls instead of 200 GB of egress.

**Step 4 is not optional.** A scratch upload left behind keeps billing for its parts, which is
exactly the problem [`leftovers`](../commands/leftovers.md) exists to find. `mito` aborts on
every path out, including the ones where something went wrong.

## The local side

Local chunks are checksummed in parallel:

1. Open the file **once**. One file descriptor no matter how many chunks, so a wide comparison
   does not run the process out of descriptors.
2. `mmap` the chunk. Zero-copy, and it pages in only what actually gets read.
3. Prefetch, then run the CRC32.
4. `munmap`, report progress.

## Both sides at once

```
                          ┌──────────────┐
                          │ Orchestrator │
                          └──────┬───────┘
                    ┌────────────┴────────────┐
                    ▼                         ▼
          ┌───────────────────┐    ┌─────────────────────┐
          │ S3 task           │    │ Local task          │
          │ UploadPartCopy    │    │ mmap + crc32_hw     │
          │ per 8 MiB chunk   │    │ per 8 MiB chunk     │
          └─────────┬─────────┘    └──────────┬──────────┘
                    │                         │
                    ▼                         ▼
             vector<uint32_t>          vector<uint32_t>
                    └────────────┬────────────┘
                                 ▼
                          compare, report
```

The two halves run at the same time, so the total is `max(local, S3)` rather than the sum. That
is worth doing because the two are limited by entirely different things. The local side waits on
disk and CPU. The S3 side waits on network round trips.

## The checksum

IEEE CRC32, the same polynomial S3 uses, computed by
[zlib-ng](https://github.com/zlib-ng/zlib-ng) using whatever the CPU offers:

| Path | Used on |
|------|---------|
| VPCLMULQDQ | x86 with AVX-512 and VPCLMULQDQ |
| PCLMULQDQ | x86 with the carry-less multiply feature bit |
| ARMv8 CRC32 | ARM64 reporting the optional CRC32 extension |
| Portable C ("braid") | Everything else |

zlib-ng checks the CPU once, on the first CRC32 call, and picks one.

**`mito` writes no CRC arithmetic of its own.** It used to. A hand-written PCLMUL path returned
wrong values for 16-byte-aligned buffers for its entire life, undetected, until it was deleted
(issue #18). Using the library is the fix.

Because the choice is made at run time inside the library, the build sets **no** architecture
flags. It used to pass `-msse4.2` or `-march=armv8-a+crc` to the whole program, which tied the
binary to the machine that built it. On ARM that was worse than untidy: CRC32 is optional before
ARMv8.1, so the binary would crash on a chip without it instead of falling back. Today a binary
built on one machine runs, and stays fast, on another.

### CRC32 is a checksum, not a cryptographic hash

It is used here for exactly one reason: S3 computes it for free on its own hardware, and that is
the entire reason a remote comparison can skip the download.

It catches accidental corruption, truncation and drift. Those are the failure modes that "did
this upload land intact?" is really asking about.

It is **no defence against someone who wants two different files to look identical**. Building a
32-bit CRC collision is trivial. If your threat model includes a motivated adversary, use a hash
designed for that instead.

### The polynomial is load-bearing

IEEE CRC32 and CRC32-C (Castagnoli) use the same instruction family on some architectures and
produce completely different numbers.

Picking the wrong one gives you a checksum that is internally consistent, passes every local
test, and disagrees with S3 on every single object.

The test suite therefore has a case whose only job is to assert the result is **not** the CRC32-C
value, next to the canonical `"123456789"` vector.

## Narrowing a mismatch

A chunk is 8 MiB, which is a vague answer to "where do these differ?"

So when two chunks disagree, that one chunk is re-analyzed at **64 KiB** granularity: 128 blocks.
The report can then name byte ranges instead of just a filename.

The two-level scheme is deliberate. Checksumming everything at 64 KiB would multiply the S3
request count by 128, on a comparison that usually finds nothing. Running the fine pass only
where the coarse pass found something keeps the common case cheap.

## Constants

| Constant | Value | Role |
|----------|-------|------|
| `CHUNK_SIZE` | 8 MiB | Unit of checksum, of parallelism, and of differential transfer |
| `BLOCK_SIZE` | 64 KiB | Granularity of mismatch analysis |
| `BLOCKS_PER_CHUNK` | 128 | `CHUNK_SIZE / BLOCK_SIZE` |

One path does not use `BLOCK_SIZE`. Two local files small enough to read whole (at most five
chunks) are compared block by block directly, at a granularity scaled to the file size: 1 KiB
below 64 KiB, up to 128 KiB above 16 MiB. The scaling aims for roughly 64 to 256 blocks whatever
the size, so the report stays readable. That is the `block_size` you will see in a report for a
small local file. Everywhere else it is 64 KiB.

8 MiB does three jobs. It is the checksum chunk, the threshold where
[`sync`](../commands/sync.md) switches to differential transfer, and the size below which an S3
object is simply downloaded and checksummed locally instead of compared remotely.

## Error handling

Fail fast, clean up always.

- A chunk-level failure aborts the whole operation rather than reporting a partial result. A
  comparison that quietly skipped a chunk would claim a match it never verified.
- Parallel errors go into an atomic counter, checked once the fan-out finishes.
- `AbortMultipartUpload` runs on every path out, including the error paths.
- Errors are logged with context through spdlog, not printed bare.

## Failure modes worth knowing

**A service without additional checksums** accepts the `UploadPartCopy` and just leaves the
header out. That fails the comparison for objects above 8 MiB. See
[S3-compatible storage](s3-compatibility.md).

**A service that answers a ranged `GET` without `Content-Range`** gets that read rejected rather
than trusted, because nothing then proves which bytes came back. This affects reads at an offset
(block narrowing, chunked downloads), not whole-object reads. See
[S3-compatible storage](s3-compatibility.md).

**A chunk count mismatch** between the two sides means the objects are different sizes. That is
reported as a difference, not an error.

**`GetBucketLocation` denied** by a cross-account policy blocks region detection. Pin the region
with `@region` instead. See [Cross-account S3](cross-account.md).
