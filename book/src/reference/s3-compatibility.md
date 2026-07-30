# S3-compatible storage

`mito` talks to Storj, MinIO, Ceph RGW and other S3-compatible services through
`--endpoint-url`. All four commands accept it: `diff`, `sync`, `rm` and `leftovers`.

```bash
mito sync ./data/ s3://my-bucket/backup/ --endpoint-url https://gateway.storjshare.io
mito rm s3://my-bucket/old/ --recursive --force --endpoint-url https://gateway.storjshare.io
```

Setting an endpoint does two things beyond changing the hostname:

1. **Switches to path-style addressing** (`https://host/bucket/key` instead of
   `https://bucket.host/key`), which these services generally require
2. **Turns off region auto-detection**, explained below

## Region detection is off with an endpoint

Auto-detection uses the AWS `GetBucketLocation` API. Other services may not implement it. And for
storage that has no regions, the question has no answer anyway.

So when `--endpoint-url` is set, the default region is used instead.

If your service cares about the region string, pin it:

```bash
mito sync ./data/ s3://my-bucket/backup/@us-1 --endpoint-url https://gateway.example.com
mito leftovers my-bucket --region us-1 --endpoint-url https://gateway.example.com
```

## The 8 MiB limit

This is the big one, so read this section before committing to a non-AWS service.

> **Objects larger than 8 MiB cannot be compared on a service that lacks additional checksums.**

The no-download comparison works by reading `x-amz-checksum-crc32` from an `UploadPartCopy`
response. That header is part of an AWS feature called additional checksums. See
[Architecture](architecture.md).

Here is what makes it awkward. A service that does not implement it **does not reject the
request**. It accepts it and simply leaves the header out.

```
   mito:     UploadPartCopy, please checksum bytes 0-8388607
   gateway:  200 OK    (and no x-amz-checksum-crc32 header)
   mito:     ...I have no checksum. I will not guess.  ──► error
```

`mito` fails the comparison with an error naming the cause, rather than reporting a match it never
actually verified.

Storj behaves this way. So do some MinIO and Ceph builds.

What that means per command:

| Operation | On a service without additional checksums |
|-----------|-------------------------------------------|
| `diff`, objects at or below 8 MiB | **Works.** They are downloaded and checksummed locally |
| `diff`, objects above 8 MiB | **Fails**, naming the missing header |
| `sync`, deciding what to transfer | **Works.** Decided by size and timestamp, no checksum needed |
| `sync`, whole-file transfer (below 8 MiB) | **Works** |
| `sync`, differential transfer (8 MiB and up) | **Fails**, same reason |
| `rm` | **Works** |
| `leftovers` | **Works** |

**So the boundary is not the service, it is the object size.** A bucket full of small objects
works completely. A bucket of large ones does not.

One softer edge: `sync` treats a missing timestamp as "transfer it". A service whose listing omits
`LastModified` therefore re-transfers everything, every run. That is correct behaviour, but it is
not cheap, and it looks like `sync` ignoring its own rules. See
[how `sync` decides](../commands/sync.md#how-it-decides-what-to-copy).

## Checking your service

Test with one large object:

```bash
mito diff ./big-file.bin s3://my-bucket/big-file.bin \
  --endpoint-url https://gateway.example.com
```

Anything over 8 MiB will either compare cleanly or report the missing checksum header.

**But that one test does not clear the whole service.** It only exercises one of the two headers
`mito` depends on. If the files match, the comparison never reaches the code that reads a byte
range at an offset. So run a second test against a file you know differs:

```bash
mito diff ./changed-file.bin s3://my-bucket/changed-file.bin \
  --endpoint-url https://gateway.example.com
```

### Ranged reads must carry `Content-Range`

Reading part of an object sends `Range: bytes=<start>-<end>`. This happens when narrowing a
mismatched chunk to 64 KiB blocks, and when downloading a large file in parallel chunks.

`mito` requires the response to say which bytes it is carrying. RFC 9110 makes `Content-Range`
mandatory on a `206`, and AWS, MinIO and Ceph RGW were all measured sending it.

A response that omits the header, or whose header names a *different* range, is rejected rather
than trusted. Here is the failure that rule prevents:

```
   mito:     give me 64 KiB starting at offset 65536
   gateway:  (ignores Range, returns the whole object)
   mito:     reads the first 64 KiB
             → right length, wrong bytes
             → reported as a difference in the wrong place
```

That is worse than an error, because it looks like a real answer.

A read starting at offset 0 is exempt. A whole-object answer to that is either the same bytes or a
different length, and a different length is already caught. So a service that omits
`Content-Range` still handles small files and whole-file downloads, and fails on large-file `sync`
and on block-level narrowing. The error names the object and the range:
`answered the range 65536-131071 without a Content-Range`.

`diff` and `sync` accept `--allow-unverified-ranges` for that case. It excuses a **missing** header
only. A `Content-Range` naming the wrong range is still refused, because that is positive evidence
the answer is not what was asked for.

Turning it on means trusting bytes you cannot place. Use it against a service you have already
established omits the header, not as a general workaround. The run logs a warning saying so.

```bash
mito diff ./data/ s3://my-bucket/data/ \
  --endpoint-url https://gateway.example.com --allow-unverified-ranges
```

## Cost accounting on non-AWS endpoints

`mito stats` prices everything at **AWS** rates.

Against a third-party service the call counts stay accurate and the dollar figure does not mean
anything. Read the operation counts and ignore the total. Or reset between runs and use it to
compare one workload against another.
