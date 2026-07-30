# Introduction

To find out whether a 200 GB object in [S3](https://aws.amazon.com/s3/) still matches the copy
on your disk, you normally have to download it. That is 200 GB of egress and an hour of
waiting, to learn one bit.

**You don't have to.** S3 will checksum a byte range of an object on its own hardware and hand
the answer back in a response header. `mito` asks it to, chunk by chunk and in parallel, and
compares what comes back against checksums computed locally with the CPU's own CRC32
instructions. The object data never crosses the network.

```
    The obvious way                     What mito does

   ┌──────────────┐                   ┌──────────────┐
   │   S3 object  │                   │   S3 object  │
   │    200 GB    │                   │    200 GB    │
   └──────┬───────┘                   └──────┬───────┘
          │                                  │
          │  200 GB over the wire            │  ~25,600 tiny checksums
          │  ~1 hour                         │  bytes stay in S3
          │  egress charges                  │  seconds to minutes
          ▼                                  ▼
   ┌──────────────┐                   ┌──────────────┐
   │  your disk   │                   │  32-bit CRCs │
   └──────┬───────┘                   └──────┬───────┘
          │                                  │
          └────────► same? ◄─────────────────┘
```

Same answer. Almost none of the cost. [Architecture](reference/architecture.md) explains how
the trick works.

## What follows from that

Once you can compare a remote file without downloading it, everything else falls out.

`mito` compares by **content**, in any direction: local to local, local to S3, S3 to S3. And
[`sync`](commands/sync.md) can use the same machinery to transfer only the parts of a big file
that actually changed, instead of the whole thing.

## What people use it for

- Checking that an upload really landed, byte for byte
- Comparing two copies of a dataset that live in different places
- Keeping a bucket in step with a directory, moving only what changed
- Finding out what your S3 API traffic is costing you
- Hunting down failed uploads that are quietly still on your bill

## Before you trust it

**This is experimental software (0.x).** It is a personal project, not operational tooling.
It does real work on real buckets, so treat it like anything else that can delete your data:
read what it prints before you type `--force`.

Three things are worth knowing up front.

**`sync` does not checksum to decide what to copy.** It looks at file size and modification
time, which is fast and cheap. It skips a file only when the sizes match *and* the destination
is newer. That catches most edits, but it trusts two clocks. A restore that sets an old
timestamp (`tar -x`, `cp -p`, `rsync -t`) looks older than the copy already there, so it gets
skipped. When you need to be sure, use [`diff`](commands/diff.md), which does checksum.

**The no-download trick needs a real S3 feature.** It depends on S3 additional checksums. Not
every S3-compatible service implements them. See
[S3-compatible storage](reference/s3-compatibility.md).

**Interfaces are settled, not frozen.** Commands, flags and exit codes are documented and
stable enough to script against. But this is still 0.x, and a version bump may change them.

## Where to start

New here? Read [Install](install.md), then [Quickstart](quickstart.md).

Already have it running and just need an answer? The command pages have the flags. The
reference pages explain why things work the way they do.
[Security and privacy](reference/security.md) covers what `mito` reads, what it writes, and
what it can destroy.

## Disclaimer

MitoSync is a personal project, written to learn hardware checksums, C++ concurrency and the
S3 API. It is provided for educational and experimental purposes only, with no warranty of any
kind. It is not intended or recommended for use in production or business-critical systems.
Evaluate it carefully and run it at your own risk on any system or bucket you rely on.

Licensed under Apache-2.0.
