# How MitoSync Reads a File Chunk, and Why It Stopped Using mmap

This is the story of a crash, a benchmark that said the opposite of what we
expected, and a change that turned out to cost nothing. You need no background
in CPUs or system calls.

## The crash

MitoSync checksums a file by splitting it into 8 MiB chunks and computing a
CRC32 for each one. To read a chunk it used `mmap`, which asks the kernel to
make part of a file appear as if it were already in memory. No copying, no
`read` calls — you just point at the bytes.

That works beautifully until the file changes underneath you.

```
   the file MitoSync is checksumming
   +--------+--------+--------+--------+
   | chunk0 | chunk1 | chunk2 | chunk3 |   <- mapped, ready to read
   +--------+--------+--------+--------+
       |
       |  meanwhile, something truncates the file:
       |  a log rotates, a build tree is cleaned,
       |  an editor rewrites a file in place
       v
   +--------+
   | chunk0 |  . . . . . . . . . . . . . .  <- chunks 1-3 no longer exist
   +--------+
                    ^
                    |
        MitoSync reads here anyway. It does not get
        an error back. It gets SIGBUS, and the whole
        process dies: "Bus error (core dumped)".
```

The nasty part is that there is nothing to check. On Linux, mapping past the end
of a file *succeeds*. Only touching the vanished pages fails, and it fails as a
signal rather than as a return value. No `if` statement can catch it.

So a perfectly ordinary event — a file shrinking during a sync — killed MitoSync
outright instead of reporting one file as failed and carrying on.

## The obvious fix, and why we hesitated

The alternative is to stop being clever: `pread` the bytes into a buffer and
checksum the buffer. A short read is just a small number instead of a signal, so
the chunk fails, the request reports failure, and the process lives.

The worry was speed. `mmap` avoids copying; `pread` copies every byte. Copying a
gigabyte to compute a checksum sounds like exactly the kind of thing you would
regret. Since MitoSync exists to checksum large files, that mattered enough to
measure rather than guess.

## The benchmark

We built a small program that mirrors the real code — same 8 MiB chunks, same
CRC32 engine, same thread pool — and changed only how the bytes arrive. Then we
measured it two ways:

- **warm**: the file is already in the operating system's page cache
- **cold**: the file has been evicted, so it really comes off the disk

Cold is the case that matters most, because a first sync is reading files nobody
has touched recently.

Three approaches went in: `mmap`, `pread` of a whole 8 MiB chunk, and `pread` in
small blocks that are checksummed as they arrive.

## The result, which was not what we expected

Numbers are throughput in MiB/s, best of five runs, on a 16-thread machine.

| 1 GiB file | mmap | pread, whole chunk | pread, 256 KiB blocks |
| --- | --- | --- | --- |
| warm | 30,554 | 9,517 (−69%) | **35,160 (+15%)** |
| cold | 2,681 | 2,961 (+10%) | **3,051 (+14%)** |

| 256 MiB file | mmap | pread, whole chunk | pread, 256 KiB blocks |
| --- | --- | --- | --- |
| warm | 24,553 | 8,513 (−65%) | **31,075 (+27%)** |
| cold | 2,897 | 2,763 (−5%) | **2,978 (+3%)** |

Two things jump out.

The naive fix really was bad. Reading a whole 8 MiB chunk and then checksumming
it was **69% slower** than `mmap` on a warm file. Our worry was justified — for
that version.

But reading in *small* blocks was **faster than `mmap`**. Not a little faster.
Faster, on every size, warm and cold.

## Why small blocks win

It comes down to how far the bytes have to travel.

```
  mmap: the whole 8 MiB chunk moves through memory twice

    disk ──> page cache ──> [   8 MiB mapping   ] ──> CRC engine
                                                 
            8 MiB is far too big to sit in the CPU's L2 cache,
            so every byte is fetched from RAM, checksummed,
            and evicted to make room for the next one.


  pread in 256 KiB blocks: the bytes stay close to the CPU

    disk ──> page cache ──> [ 256 KiB buffer ] ──> CRC engine
                                   ^        |
                                   +--------+
                             small enough to live in L2,
                             so the CRC reads it back while
                             it is still hot. The "extra" copy
                             is cheaper than the trip to RAM.
```

The copy we were afraid of turns out to be cheaper than the memory traffic it
replaces — as long as the buffer is small.

We can check that this is really the explanation: 1 MiB blocks are about **35%
slower** than 256 KiB blocks. If the win came from `pread` itself, bigger blocks
would be better, because they mean fewer system calls. They are worse. So the
cache is doing the work, exactly as the diagram claims.

## What we chose

256 KiB blocks with `pread`. It was best or near-best in every column, and it
makes 32 system calls per chunk instead of 128, which is kinder to slower
storage than the 64 KiB variant.

## An honest caveat

None of this speeds up MitoSync in a way you will notice. A 1 GiB local diff
takes 0.07 seconds either way, because checksumming was never where that time
went — enumeration, comparison, and the network dominate.

The benchmark did not tell us to make this change. It told us the change was
**free**, which is a different and more useful thing. The reason to make it is
the signal that no longer arrives.

## The road not taken

You can keep `mmap` and survive truncation, by installing a `SIGBUS` handler and
jumping out of it when the fault hits. It is a known technique. We did not use
it, for three reasons:

- A signal handler is process-wide. It would catch bus errors from anywhere,
  including a genuine hardware memory fault, and quietly report them as "this
  chunk failed" — losing information you would very much want.
- Jumping out of a signal handler in C++ skips destructors, so anything holding
  a resource at the moment of the fault leaks it.
- It is difficult to get right across a thread pool.

That is a lot of delicate machinery to defend a performance advantage that the
benchmark says does not exist.

## A bonus we did not plan

`mmap` requires its offset to be page-aligned. That meant an unusual chunk size —
say 1000 bytes — made every chunk after the first fail to map, and the whole
request had to be abandoned. It was a real limitation with a real bug report
against it.

`pread` has no alignment rule. Any chunk size simply works now, and the
limitation disappeared along with the crash.

## Where the code lives

- `src/crc32_chunks.cpp` — `compute_crc32_chunks_boost_asio`, the read loop
- `tests/test_file_utils.cpp` — `LocalCRC32Test.AFileTruncatedMidReadFailsInsteadOfKillingTheProcess`
  shrinks a file mid-read and asserts MitoSync survives. It dies with exit code
  135 on the old implementation, which is how we know it tests the right thing.
