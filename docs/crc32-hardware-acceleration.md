# CRC32 Hardware Acceleration, Told as a Small Journey

This is a friendly tour of how MitoSync computes CRC32 checksums quickly on
different machines. You do not need a background in CPUs, checksums, or S3.

The short version:

- MitoSync breaks a file into chunks.
- Each chunk gets a small fingerprint called a CRC32.
- Local chunks are checksummed by the local CPU when possible.
- S3 chunks are checksummed by S3 itself when possible.
- The two lists of fingerprints are compared instead of moving whole files.

```
       local file                              S3 object
   +----------------+                    +----------------+
   | chunk 0        |                    | chunk 0        |
   | chunk 1        |                    | chunk 1        |
   | chunk 2        |                    | chunk 2        |
   +-------+--------+                    +-------+--------+
           |                                     |
           v                                     v
   local CRC32 engine                    S3 CRC32 header
           |                                     |
           v                                     v
   [a1, b2, c3, ...]             vs      [a1, b2, c3, ...]
           \_____________________________________/
                           |
                           v
                    equal means match
```

## The Story

Imagine a librarian named Mira who has two giant books.

One book sits on her desk. The other is locked in a warehouse across town. She
wants to know whether the books are identical, but carrying the warehouse book
back would be slow and expensive.

So Mira uses page seals.

For every 8 MiB section of the desk book, she asks her assistant to make a
small seal. The seal is only 32 bits, so it is tiny, but if a page changes by
accident the seal almost always changes too.

Then she calls the warehouse and says:

"Do not send me the pages. Just make the same seal for section 17 and read the
seal number back to me."

Now she has two rows of seal numbers.

```
desk book:       7A92D108  0192CAFE  44BB1200  AAAAAAAA
warehouse book:  7A92D108  0192CAFE  44BB1200  AAAAAAAA
                 same      same      same      same
```

If every seal matches, the books match for MitoSync's purpose: detecting
accidental drift, truncation, wrong uploads, and changed bytes.

CRC32 is that seal.

## What CRC32 Is, Without the Math

CRC32 is a checksum. It reads bytes and produces a 32-bit number.

```
bytes in:
  48 65 6c 6c 6f
       |
       v
CRC32 machine
       |
       v
32-bit seal:
  f7d18982
```

A checksum is not a secret password and not a cryptographic hash. It is a smoke
alarm, not a bank vault. It is good at catching accidents. It is not meant to
stop an attacker who deliberately tries to fool it.

MitoSync uses IEEE CRC32 because that is the CRC32 flavor S3 exposes in
`x-amz-checksum-crc32`.

That detail matters.

There are multiple CRC families. One common hardware instruction on x86 is
called `crc32`, but it computes CRC32-C, not IEEE CRC32. Same name, different
seal.

```
same bytes
   |
   +--> IEEE CRC32   -> what S3 returns, what MitoSync needs
   |
   +--> CRC32-C      -> a different checksum, not interchangeable
```

The tests guard this with the classic input `123456789`:

```
IEEE CRC32:  0xCBF43926   wanted
CRC32-C:     0xE3069283   wrong for MitoSync
```

## The Local Path

The local path starts in `src/crc32_chunks.cpp`.

MitoSync opens a file, splits it into chunks, reads each chunk in 256 KiB
blocks with `pread`, and passes those bytes to `crc32_hw()`.

```
file on disk
    |
    v
open()
    |
    v
split into chunk ids
    |
    v
pread one block
    |
    v
crc32_hw(bytes, length)
    |
    v
store uint32_t result
```

Why `pread`?

Because a file can shrink while MitoSync is reading it. With `mmap`, touching a
page that vanished can raise `SIGBUS` and kill the process. With `pread`, the
same event is an ordinary short read that can be reported as a failed file.

The chunk worker is deliberately simple:

```
for each chunk:
    read 256 KiB blocks with pread
    ask crc32_hw() to extend the checksum
    stop cleanly on short read or read error
```

If there are only a few chunks, MitoSync does that sequentially to avoid thread
pool overhead. If there are many chunks, it posts work to a Boost.Asio thread
pool.

```
few chunks:

chunk 0 -> CRC
chunk 1 -> CRC
chunk 2 -> CRC

many chunks:

          +--> worker A -> chunk 0 -> CRC
thread    +--> worker B -> chunk 1 -> CRC
pool      +--> worker C -> chunk 2 -> CRC
          +--> worker D -> chunk 3 -> CRC
```

## The Dispatcher

The function `crc32_hw()` lives in `src/crc32_hw.cpp`.

It is the small door every local chunk walks through. Behind that door there is
now exactly one line of code: a call into zlib-ng, which picks the right
implementation for the CPU the program is running on.

```
                crc32_hw(data, length)
                         |
                         v
                 zng_crc32_z(0, data, length)
                         |
     zlib-ng looks at the CPU, once, on the first call
                         |
        +----------------+----------------+
        |                |                |
        v                v                v
  x86 PCLMULQDQ    ARMv8 CRC32     portable C ("braid")
  or VPCLMULQDQ
```

Notice what is *not* in that picture: compile flags. The choice is made at run
time from what the CPU reports, so a binary built on one machine stays correct
on another - and if the machine has the fast instructions, it uses them.

The public header is tiny:

```cpp
bool has_hw_crc32();
const char* hw_crc32_name();
uint32_t crc32_hw(const uint8_t* data, size_t length);
```

Callers do not need to know the CPU details. They call `crc32_hw()` and get the
right CRC32 value, or the operation fails a test.

`has_hw_crc32()` and `hw_crc32_name()` are only for the debug log. They ask the
CPU what it can do, rather than repeating what the compiler was told, because
the old version claimed `"x86 PCLMUL"` on builds that contained no PCLMUL code
at all (issue #22). Nothing consults them to decide how a CRC is computed.

## Why a Library Instead of Our Own Instructions

MitoSync used to write this code itself: ARM ACLE intrinsics on ARM64, and a
hand-written PCLMUL block on x86. Both are gone. The story of why is worth
telling, because it is the reason for the current design.

x86 has a naming trap. It has an instruction named `crc32`, enabled by SSE4.2,
but that instruction computes CRC32-C. MitoSync needs IEEE CRC32.

```
x86 SSE4.2 crc32 instruction
          |
          v
      CRC32-C
          |
          v
wrong seal for S3's x-amz-checksum-crc32
```

So a correct fast x86 implementation needs a more advanced technique: PCLMUL,
short for carry-less multiply. PCLMUL is not a one-instruction checksum. It
needs folding and reduction steps.

```
correct PCLMUL-style IEEE CRC32, simplified:

bytes
  |
  v
fold big blocks
  |
  v
fold smaller blocks
  |
  v
reduce by CRC polynomial
  |
  v
final IEEE CRC32
```

The hand-written version got that wrong. It multiplied the running CRC by each
block and kept the low word, which is not a CRC at all, and it produced wrong
values for any 16-byte-aligned buffer of 64 bytes or more (issue #18). Worse, it
was invisible: the default build passed `-msse4.2`, which does not define
`__PCLMUL__`, so the broken branch was compiled out and never tested - while a
`-march=native` build would have run it for every chunk of every file.

That is a lot of delicate maths, per architecture, to keep verified. So
MitoSync stopped writing it and adopted **zlib-ng** instead (issue #50):

- Zlib license, same posture as the rest of the project.
- Already packaged in vcpkg, so it is one line in `vcpkg.json`.
- Accelerated CRC32 for SSE2, PCLMULQDQ, VPCLMULQDQ, ARMv8, Power8, IBM Z,
  LoongArch and RISC-V, chosen at run time.
- Its own CI and fuzzing, upstream.

MitoSync calls the prefixed `zng_` API rather than zlib-ng's "compat" mode,
which would have zlib-ng stand in for zlib everywhere. aws-sdk-cpp already
depends on real zlib, and the prefixed API lets both live in one binary:

```cpp
return zng_crc32_z(0, data, length);
```

That `_z` suffix matters. It takes a `size_t` length. The old call cast the
length to zlib's 32-bit `uInt`, so a buffer of 4 GiB or more was checksummed
over `length mod 2^32` bytes - and for a single-part upload that wrong checksum
went to S3 as `x-amz-checksum-crc32` (issues #48 and #21).

### What it costs and what it buys

Measured on one core of an i9-9900K (PCLMULQDQ, no AVX512), 8 MiB buffer:

```
zlib     4.7 GB/s
zlib-ng 24.8 GB/s        about 5x
```

End to end, over a warm 2 GiB file, the chunked path goes from 31 GB/s to
35.6 GB/s - only about 15%, because the thread pool spreads chunks across all
cores and then memory bandwidth, not CRC arithmetic, is the limit. On a single
core - the common case when a directory comparison already has every core busy
with a different file - the same path goes from 4.4 GB/s to 15.2 GB/s.

The cost is one more dependency. The benefit is that nobody has to maintain
folding CRC maths in this repo again.

### The build no longer raises the ISA baseline

Because zlib-ng compiles its own SIMD paths and selects among them at run time,
`CMakeLists.txt` no longer passes `-msse4.2` or `-march=armv8-a+crc`. The ARM
flag was actively harmful: CRC32 instructions are optional before ARMv8.1, so a
binary built with that flag would crash with an illegal instruction on an
ARMv8.0 part rather than fall back (issue #49).

`mito_tests_crc32_native` still builds the CRC tests with `-march=native`. There
is no ISA-gated code left in MitoSync for it to wake up, but it stands guard
against someone reintroducing some: a CRC that only misbehaves when the compiler
may use the host's full instruction set is exactly the shape of issue #18.

## Other Architectures: The Reliable Old Road

On a CPU with no accelerated path, zlib-ng falls back to its portable C
implementation - a "braid" of several CRC streams that is still faster than the
classic table lookup.

```
unknown CPU
    |
    v
zlib-ng braid (portable C)
    |
    v
correct IEEE CRC32
```

Same call, same answer, no special case in MitoSync's code. It may be slower
than a hardware path, but it keeps the promise that correctness comes first.

## The Remote S3 Path

The local CPU only handles local files. For S3 objects, MitoSync tries not to
download object bytes at all.

Instead, it asks S3 to checksum a byte range.

```
MitoSync:
  "S3, please copy bytes 8388608..16777215 inside your own service,
   and give me the CRC32 checksum for that part."

S3:
  "Here is x-amz-checksum-crc32."
```

The rough flow is:

```
S3 object
   |
   v
UploadPartCopy for one range
   |
   v
S3 computes CRC32 internally
   |
   v
response header: x-amz-checksum-crc32
   |
   v
MitoSync decodes 4 bytes into uint32_t
```

For large objects, this is the key trick:

```
bad old way:

S3 object, 200 GB
    |
    v
download 200 GB
    |
    v
checksum locally

MitoSync way:

S3 object, 200 GB
    |
    v
ask S3 for one CRC32 per 8 MiB range
    |
    v
receive 4 bytes per chunk
```

MitoSync still needs S3-compatible storage to support this header. Some
gateways accept the copy request but omit the checksum header. In that case the
code must fail rather than invent a checksum.

## How the Pieces Meet

Here is the whole comparison as one picture.

```
                 LOCAL SIDE                         REMOTE SIDE

             file on disk                         object in S3
                  |                                   |
                  v                                   v
             split chunks                        split ranges
                  |                                   |
                  v                                   v
          pread chunk blocks                  UploadPartCopy range
                  |                                   |
                  v                                   v
            crc32_hw()                        x-amz-checksum-crc32
                  |                                   |
                  v                                   v
       +-------------------+              +-------------------+
       | local CRC list    |              | S3 CRC list       |
       | [c0, c1, c2, ...] |              | [c0, c1, c2, ...] |
       +---------+---------+              +---------+---------+
                 |                                  |
                 +---------------+------------------+
                                 |
                                 v
                       compare index by index
                                 |
                +----------------+----------------+
                |                                 |
                v                                 v
          all entries equal                 one entry differs
                |                                 |
                v                                 v
           files match                  chunk N is different
```

This gives MitoSync two useful abilities:

- `diff` can compare local and S3 content without downloading the S3 object.
- differential `sync` can avoid retransferring chunks that already match.

## Why the Tests Matter

CRC code is easy to get almost right and still be wrong.

The tests compare `crc32_hw()` against known answers and against zlib - which,
now that the CRC comes from zlib-ng, means two independent implementations are
checked against each other. They also include cases that catch common mistakes:

- empty input
- `"123456789"` returning `0xCBF43926`
- many sizes and alignments
- 8, 4, 2, and 1 byte tails
- proof that the result is not CRC32-C
- a 4 GiB + 100 byte buffer, so a truncated length cannot hide
- S3's base64, big-endian checksum format
- thread safety under concurrent calls

```
implementation
      |
      v
known vectors
      |
      v
zlib reference
      |
      v
S3 format checks
      |
      v
"safe enough to use"
```

The important lesson is that hardware acceleration is allowed only when it
produces the same IEEE CRC32 as the boring reference implementation.

Fast and wrong is worse than slow and correct.

## Code Map

Use this map when reading the source.

```
include/crc32_hw.h
    public functions:
      has_hw_crc32()
      hw_crc32_name()
      crc32_hw()

src/crc32_hw.cpp
    crc32_hw() -> zng_crc32_z() from zlib-ng
    runtime CPU detection, for the debug log only

src/crc32_chunks.cpp
    opens local files
    splits them into chunks
    reads each chunk with pread
    calls crc32_hw()
    runs chunks sequentially or in a thread pool

src/s3_utils.cpp and S3 client code
    asks S3 for per-range x-amz-checksum-crc32
    decodes remote CRC32 values

tests/test_crc32.cpp
    proves all paths produce IEEE CRC32
```

## The Rule to Remember

MitoSync's CRC32 acceleration is not "use whatever instruction sounds like
CRC32."

It is:

```
1. Choose the checksum flavor S3 uses: IEEE CRC32.
2. Split large data into chunks.
3. For local data, use the fastest correct local engine.
4. For S3 data, ask S3 for its checksum header.
5. Compare the tiny checksum lists.
6. If a fast path cannot prove correctness, fall back.
```

That is the whole story: Mira does not carry the warehouse book across town.
She compares seals, and she trusts only seals made by the right stamp.

## Where the Bytes Come From

This tour covers how a chunk is *checksummed*. How it is *read* off the disk in
the first place is a separate small story, with its own crash and its own
surprising benchmark: see `reading-file-chunks.md`.
