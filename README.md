# MitoSync

[![CI](https://github.com/echemythia/mitosync/actions/workflows/ci.yml/badge.svg)](https://github.com/echemythia/mitosync/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/echemythia/mitosync/branch/main/graph/badge.svg)](https://codecov.io/gh/echemythia/mitosync)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey.svg)](#platform-support)

**Documentation:** <https://echemythia.github.io/mitosync/>

To find out whether a 200 GB object in [S3](https://aws.amazon.com/s3/) still matches the copy
on your disk, you normally have to download it. That is 200 GB of egress and an hour of
waiting, to learn one bit.

**You don't have to.** S3 will checksum a byte range of an object on its own hardware and hand
the answer back in a response header. `mito` asks it to, chunk by chunk and in parallel, and
compares what comes back against checksums computed locally with the CPU's own CRC32
instructions. The object data never crosses the network.

```
      ./data/big.bin                                 s3://my-bucket/big.bin
      200 GB on disk                                   200 GB in the cloud
             │                                                  │
             │  split into 8 MiB chunks                         │
             ▼                                                  ▼
 ┌───────────────────────┐                        ┌────────────────────────────┐
 │ pread chunk N         │                        │ UploadPartCopy, no download│
 │ CRC32 via PCLMULQDQ / │                        │   Range: bytes=N*8Mi-...   │
 │ ARMv8 CRC instructions│                        │ S3 checksums it, replies:  │
 │                       │                        │   x-amz-checksum-crc32     │
 │ reads 200 GB locally  │                        │ 4 bytes back per chunk     │
 └───────────┬───────────┘                        └─────────────┬──────────────┘
             │                                                  │
             │      both sides run concurrently                 │
             ▼                                                  ▼
   [ crc32, crc32, ... ]             vs               [ crc32, crc32, ... ]
             └────────────────────────┬─────────────────────────┘
                                      ▼
                          all equal → the copies match
             any differ → which chunk, narrowed to 64 KiB blocks

           25,600 small API calls.  0 bytes of object data moved.
```

Everything else follows from that. `mito` compares by **content** in any direction: local to
local, local to S3, S3 to S3. And `sync` uses the same machinery to send only the parts of a big
file that actually changed.

One caveat worth stating early. CRC32 is a checksum, not a cryptographic hash. It catches
accidental corruption, truncation and drift, which is what "did my upload land intact" really
asks. It is no defence against someone who *wants* two files to look identical — colliding a
32-bit CRC on purpose is easy. Use it accordingly.

**Contents:** [Status](#status) · [Install](#install) · [Quickstart](#quickstart) ·
[Usage](#usage) · [Exit codes](#exit-codes) · [How it works](#how-it-works) ·
[S3-compatible storage](#s3-compatible-storage-storj-minio-ceph) ·
[Cross-account S3](#cross-account-s3) · [Platform support](#platform-support) ·
[Build from source](#build-from-source) · [Development](#development) ·
[Layout](#layout) · [Security and privacy](#security-and-privacy) ·
[Disclaimer](#disclaimer) · [License](#license)

## Status

**Experimental (0.x).** This is a personal project, not operational tooling. It does real work on
real buckets, so treat it like anything else that can delete your data: read what it prints before
you type `--force`.

Four things are worth knowing before you rely on it.

**`sync` does not checksum to decide what to copy.** It looks at size and modification time, which
is fast and cheap. It skips a file only when the sizes match *and* the destination is newer. That
catches most edits, but it trusts two clocks. A restore that puts an old timestamp back (`tar -x`,
`cp -p`, `rsync -t`) looks older than the copy already there, so it gets skipped. Use `diff` when
you need certainty.

**The no-download trick needs a real S3 feature.** It depends on S3 additional checksums. Not
every S3-compatible service implements them — see
[S3-compatible storage](#s3-compatible-storage-storj-minio-ceph).

**Interfaces are settled, not frozen.** Commands, flags and exit codes are documented and stable
enough to script against. But this is 0.x, and a version bump may change them.

**EC2 instance roles do not work today.** `mito` disables instance metadata lookup at startup to
save a timeout, and currently does so even if you asked otherwise. That is a bug rather than a
design choice ([#105](https://github.com/echemythia/mitosync/issues/105)). Use environment
variables, a named profile, or an ECS task role.

The [Disclaimer](#disclaimer) below is the short version, and it is the one that governs.

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/echemythia/mitosync/main/install.sh | sh
```

This works out your platform, checks the release asset against its `.sha256`, and runs the no-sudo
installer. It installs under `$HOME/.local`.

To choose a different prefix you own:

```bash
curl -fsSL https://raw.githubusercontent.com/echemythia/mitosync/main/install.sh | sh -s -- --prefix "$HOME/.local"
```

To pin a specific release:

```bash
curl -fsSL https://raw.githubusercontent.com/echemythia/mitosync/main/install.sh | MITO_VERSION=v0.7.0 sh
```

Or pick the asset for your machine yourself:

```bash
# Linux x86_64
curl -fsSL https://github.com/echemythia/mitosync/releases/latest/download/mito-linux-x86_64.install.sh | sh

# Linux ARM64
curl -fsSL https://github.com/echemythia/mitosync/releases/latest/download/mito-linux-aarch64.install.sh | sh

# macOS Apple Silicon
curl -fsSL https://github.com/echemythia/mitosync/releases/latest/download/mito-macos-arm64.install.sh | sh
```

macOS releases are **Apple Silicon only**. On an Intel Mac, build from source.

Downloading first and running it afterwards works the same way:

```bash
sh ./mito-<version>-linux-x86_64.install.sh
sh ./mito-<version>-linux-x86_64.install.sh --prefix "$HOME/.local"
```

The binary lands at `$HOME/.local/bin/mito` and the docs under `$HOME/.local/share/doc/mito`. Make
sure `$HOME/.local/bin` is on your `PATH`.

On Linux there are system packages too:

```bash
sudo apt install ./mito-<version>-linux-x86_64.deb
# or
sudo dnf install ./mito-<version>-linux-x86_64.rpm
```

Those install `mito` as `/usr/bin/mito`. Tarballs are published as well, if you would rather place
things by hand.

Building from source is straightforward:

```bash
git clone --recurse-submodules git@github.com:echemythia/mitosync.git
cd mitosync
./bootstrap.sh                       # one-time: set up vcpkg
cmake --preset linux-release         # or macos-release
cmake --build build
sudo cmake --install build --prefix /usr/local
```

Full detail is in [Build from source](#build-from-source).

Credentials come from the standard AWS chain: environment variables, `~/.aws/credentials`
including named profiles, or an ECS task role. `mito` reads them and stores none of its own.

## Quickstart

```bash
# Do these two directories hold the same bytes?
mito ./data/ /mnt/backup/data/

# Does what I uploaded match what is on disk?
mito diff ./data/ s3://my-bucket/data/

# Same question, with a report a script can read
mito diff ./data/ s3://my-bucket/data/ -o results.json

# Make the bucket match the directory, but show me first
mito sync ./data/ s3://my-bucket/backup/ --dry-run

# Now really do it, and remove what is no longer at the source
mito sync ./data/ s3://my-bucket/backup/ --delete

# What did all that cost me in S3 API calls?
mito stats
```

## Usage

```
mito [COMMAND] [OPTIONS]
```

| Command | Purpose |
|---------|---------|
| `diff` | Compare two files or directories by CRC32. The default when no command is given |
| `sync` | Synchronize a directory to or from S3, or between two S3 prefixes |
| `rm` | Delete S3 objects |
| `stats` | Show accumulated S3 API usage and cost estimates |
| `leftovers` | Find and abort incomplete multipart uploads |

Run `mito <command> --help` for per-command options, and `mito --version` (or `-V`) for the
version. The version flag is read from `argv[1]` only, like `git --version`, so
`mito rm --version <url>` stays the typo it is instead of becoming a version request.

Paths come in four shapes:

```
  /home/me/report.pdf        one local file
  /home/me/data/             a local folder  (note the slash)
  s3://my-bucket/report.pdf  one S3 object
  s3://my-bucket/data/       an S3 folder    (note the slash)
```

The trailing slash is what tells `mito` whether you mean one file or a whole tree. Add `@region`
to an S3 path to pin the region instead of letting `mito` detect it.

### Compare

```bash
mito /path/to/dir1/ /path/to/dir2/                        # two directories, recursively
mito ./data/ s3://my-bucket/data/ -o results.json         # local against S3, with a report
mito file.bin s3://my-bucket/file.bin@eu-west-1           # one object, region pinned
```

The report format follows the `-o` extension: `.json`, `.csv` or `.txt`. When a file differs, the
report does not just name the file. It names the differing 8 MiB chunks and narrows each one to
64 KiB blocks, so you learn *where* the copies diverged.

### Sync

Direction comes from argument order. Source first, destination second.

```bash
mito sync ./data/ s3://my-bucket/backup/            # upload
mito sync s3://my-bucket/backup/ ./data/            # download
mito sync s3://my-bucket/a/ s3://my-bucket/b/       # S3 to S3, copied inside AWS
mito sync ./data/ s3://my-bucket/backup/ --delete   # upload, prune extras at destination
mito sync ./data/ s3://my-bucket/backup/ --dry-run  # preview only
```

A file is skipped only when **the sizes match and the destination is newer than the source**.
Everything else transfers.

No checksum is read to make that decision — both signals come from the listing each side already
produced, which is what keeps a sync over a huge tree fast. Size alone would not be enough: an
in-place edit that keeps the length identical moves only the timestamp.

Files of 8 MiB or more then use differential transfer, sending only the chunks whose CRC32 differs.

Two consequences worth knowing. A restore that moves a timestamp backwards gets skipped, as
described in [Status](#status). And a run whose listing came back incomplete is **refused
outright** rather than acted on — a partial listing looks exactly like a smaller tree, and under
`--delete` the entries it failed to mention look exactly like files to remove. `diff` refuses for
the same reason.

### Delete

`rm` deletes nothing until you pass `--force`. Without it you get a preview. A prefix also needs
`--recursive`, so pointing `rm` at a prefix by accident is an error rather than a mass delete.

```bash
mito rm s3://my-bucket/file.txt --force
mito rm s3://my-bucket/old-data/ --recursive --force
mito rm s3://my-bucket/old-data/                    # preview, deletes nothing
```

### Cost and cleanup

```bash
mito stats                                        # cumulative API calls and an estimated bill
mito stats --json                                 # the same, machine-readable
mito stats --reset                                # start counting again

mito leftovers my-bucket                          # list incomplete multipart uploads
mito leftovers my-bucket --older-than 7d --abort  # abort the stale ones
```

Incomplete multipart uploads matter because S3 keeps charging for their parts until they are
aborted, and they appear in **no object listing**. A failed 200 GB upload is a silent, recurring
charge that nothing except the bill will show you.

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | Success. For `diff`, everything matched |
| `1` | A mismatch was found, or the operation failed |
| `2` | Bad command line or configuration. Nothing was attempted |
| `130` | Interrupted by Ctrl-C (128 + 2) |

Code `1` deliberately does two jobs: "the copies differ" and "something went wrong". A script that
needs to tell those apart should read the report file, where they are separate counters, rather
than the exit status.

A `2` always means nothing happened, so a retry loop should treat it as terminal.

## How it works

A comparison runs in three phases, each parallel in a different way.

**1. Discovery.** Both sides are walked breadth-first, one level at a time, with the directories at
each level spread across a thread pool. Going level by level keeps memory bounded to the current
and next level, and it keeps symlink loop detection tractable. S3 listing is capped at 80 workers
to stay under rate limits.

**2. File comparison.** Files are compared through a sliding window whose width adapts to measured
throughput. It starts from the shape of the workload — many small files want breadth, a few large
files want depth — doubles while throughput keeps improving by more than 10%, then locks once two
measurements in a row come back flat.

That adaptation matters more than it sounds. The right concurrency for 100,000 tiny objects and for
four 50 GB objects differ by orders of magnitude, and no fixed number serves both.

**3. Chunk comparison.** Each file is cut into 8 MiB chunks and each chunk is checksummed on its
own.

- **Locally**, the chunk is read with `pread` and handed to
  [zlib-ng](https://github.com/zlib-ng/zlib-ng), which checks the CPU on first use and takes
  PCLMULQDQ or VPCLMULQDQ on x86, the ARMv8 CRC32 instructions on ARM64, or portable C anywhere
  else. Using `pread` keeps a shrinking file a normal read failure rather than a fatal signal.
- **In S3**, the chunk is never downloaded. `mito` issues an `UploadPartCopy` against the object
  and reads `x-amz-checksum-crc32` off the response, which is S3 computing that byte range's CRC32
  on its own hardware. The scratch multipart upload is always aborted afterwards.

Both sides run at the same time, so a local-to-S3 comparison costs `max(local, S3)` rather than the
sum.

The checksum is IEEE CRC32, the same polynomial S3 uses. There is a test whose only job is to prove
it has not accidentally become CRC32-C, which uses the same instructions and produces completely
different numbers.

For the full picture see
[Architecture](https://echemythia.github.io/mitosync/reference/architecture.html) and
[Parallelism](https://echemythia.github.io/mitosync/reference/parallelism.html).

## S3-compatible storage (Storj, MinIO, Ceph)

`--endpoint-url` points every S3 side at a non-AWS service and switches to path-style addressing,
which these services generally require. `diff`, `sync`, `rm` and `leftovers` all accept it.

```bash
mito sync ./data/ s3://my-bucket/backup/ --endpoint-url https://gateway.storjshare.io
mito rm s3://my-bucket/old/ --recursive --force --endpoint-url https://gateway.storjshare.io
```

Region auto-detection is skipped when an endpoint is set. Detection uses the AWS
`GetBucketLocation` API, which these services may not implement and which means nothing for storage
that has no regions, so the default region is used instead. If your service cares, pin it with
`@region` on the URL. (`leftovers` takes a bare bucket name rather than a URL, so it has a
`--region` flag for the same job.)

> **Limitation: objects larger than 8 MiB.** The no-download comparison reads
> `x-amz-checksum-crc32` from `UploadPartCopy`, an AWS additional-checksums feature. A service that
> does not implement it accepts the request and simply leaves the header out. Storj behaves this
> way, as do some MinIO and Ceph builds.
>
> So `diff` and differential `sync` fail there for objects above the chunk size, with an error
> naming the cause rather than reporting a match nothing verified. Objects at or below 8 MiB are
> unaffected: they are downloaded and checksummed locally. `rm`, `leftovers` and `sync`'s
> size-and-timestamp decisions work normally. **The boundary is the object size, not the service.**
>
> A second header matters for the same reason. A ranged `GET` must answer with `Content-Range`, or
> `mito` fails the read rather than trusting bytes it cannot place. RFC 9110 requires it on a
> `206`, and AWS, MinIO and Ceph RGW were all measured sending it, so this is a guard rather than a
> known incompatibility. It applies to reads at an offset — block narrowing and chunked large-file
> downloads — not to whole-object reads. `--allow-unverified-ranges` accepts a *missing* header if
> you meet a service that needs it; a header naming a *different* range is refused either way. See
> [S3-compatible storage](https://echemythia.github.io/mitosync/reference/s3-compatibility.html).

## Cross-account S3

`sync` and `diff` accept `--source-profile` and `--dest-profile`, so each S3 side can authenticate
as a different AWS named profile. Leave one off to use the default chain.

| Flag | `sync` | `diff` |
|------|--------|--------|
| `--source-profile <name>` | S3 source (S3 to S3, or download) | source1, if it is S3 |
| `--dest-profile <name>` | S3 destination (S3 to S3, or upload) | source2, if it is S3 |

```bash
# Each side uses its own AWS profile
mito sync s3://acctA-bucket/data/ s3://acctB-bucket/data/ \
  --source-profile acctA --dest-profile acctB

mito diff s3://acctA-bucket/data/ s3://acctB-bucket/data/ \
  --source-profile acctA --dest-profile acctB

# If neither profile can read the other's bucket location, pin regions
mito sync s3://acctA-bucket/data/@us-east-1 s3://acctB-bucket/data/@eu-west-1 \
  --source-profile acctA --dest-profile acctB
```

> **An S3-to-S3 copy needs a policy on the source bucket.** The copy is one API call, sent to the
> destination, naming the source in a header. There is only one set of credentials on that request
> and they belong to the **destination** profile, so the destination principal has to be able to
> read the source bucket.
>
> `--source-profile` authenticates the source *listing*. It cannot authenticate the copy. A real
> cross-account copy therefore also needs a source bucket policy granting the destination principal
> `s3:GetObject` and `s3:ListBucket`. Cross-account `diff`, upload and download need no such grant,
> because there the bytes pass through your machine and each side is authenticated separately.

## Platform support

| Platform | Architecture | Accelerated CRC32 |
|----------|--------------|-------------------|
| Linux | x86_64 | PCLMULQDQ, or VPCLMULQDQ with AVX-512 |
| Linux | ARM64 | ARMv8 CRC32 |
| macOS | ARM64 (Apple Silicon) | ARMv8 CRC32 |

zlib-ng picks among these at run time from what the CPU reports, so a binary is not tied to the
machine that built it. Anything else runs zlib-ng's portable C implementation, which is correct but
slower. There is no Windows build.

## Build from source

**Prerequisites:** CMake 3.19 or newer for the presets (3.15 without them), a C++17 compiler, Ninja
and the vcpkg submodule.

```bash
git clone --recurse-submodules git@github.com:echemythia/mitosync.git
cd mitosync

./bootstrap.sh                 # one-time: set up vcpkg and fetch dependencies

cmake --preset linux-release   # or macos-release
cmake --build build

./build/mito --help
```

vcpkg resolves these from `vcpkg.json`, so you do not install them yourself:

| Library | Purpose |
|---------|---------|
| aws-sdk-cpp (s3 only) | S3 API, multipart operations |
| Boost.Asio | Thread pools, async scheduling |
| OpenSSL | TLS for the AWS SDK |
| spdlog | Structured logging |
| nlohmann-json | JSON reports and stats |
| zlib-ng | CRC32, accelerated per CPU at run time |
| zlib | Pulled in by the AWS SDK; the CRC tests use it as an independent reference |
| GoogleTest | Test suite |

Release builds enable `-O3`, link-time optimization, `-fstack-protector-strong` and full RELRO on
Linux. No architecture flags are set on purpose: zlib-ng compiles its own SIMD paths and chooses
between them at run time, so the build never raises the ISA baseline.

## Development

```bash
cmake --preset linux-release
cmake --build build

ctest --test-dir build --output-on-failure       # the whole suite

./build/mito_tests                              # one binary of it
./build/mito_tests --gtest_filter=CRC32Test.*   # one group within that binary
./build/mito_tests --gtest_list_tests           # what that binary holds
```

The suite is split across roughly twenty `mito_tests_*` binaries, one per area, so `ctest` is the
only thing that runs all of it. `./build/mito_tests` is simply the largest of them (CRC32, URL
parsing, file utils, the S3 mock, directory comparison). `sync`, `rm`, `leftovers`, metrics and
pricing each live in their own binary. `ls build/mito_tests_*` lists them.

**`ctest -j` is safe.** Every fixture's scratch directory carries the process id and the test name,
so parallel jobs — and two copies of the suite side by side — cannot remove each other's files.

One test, `BucketListStateTest.CancelDuringProgressUpdate`, asserts that a cancellation 50 µs in
lands before four worker threads finish. It can still fail on a machine busy enough to delay that,
and passes on a rerun.

Coverage and thread-sanitizer builds:

```bash
./scripts/coverage.sh                # gcov/lcov report
cmake --preset macos-tsan            # ThreadSanitizer build
```

CI builds and tests on Linux for every push and pull request against `main`.

### Cutting a release

`scripts/package-release.sh` builds, tests, strips and packages an artifact for whatever machine you
run it on. Then it extracts the tarball again and runs the binary out of it before declaring
success. It also produces a no-sudo self-extracting installer, and on Linux a `.deb` and `.rpm`
from the same staged binary:

```bash
./scripts/package-release.sh          # → dist/mito-<version>-<platform>.*[.sha256]
```

Each tarball carries the binary, `LICENSE`, `README.md`, `THIRD-PARTY-NOTICES.md` and a
`BUILD-INFO.txt` recording what that binary needs at run time: the minimum glibc or macOS version,
and its dynamic dependencies.

On Linux the vcpkg dependency stack and the GCC runtime libraries are linked statically, while
glibc-family system libraries stay dynamic. On macOS the binary is stripped and then re-signed
ad-hoc, because stripping invalidates the signature Apple Silicon requires.

Pushing a `vMAJOR.MINOR.PATCH` tag runs the same script on GitHub-hosted runners and publishes the
tarballs, installers and Linux packages. The tag must match the version in `CMakeLists.txt` and
`vcpkg.json` or the workflow refuses to build. A `-rcN` tag rehearses the whole thing and publishes
nothing.

Three platforms are required and the release fails without them. The Linux arm64 set is
best-effort, because hosted arm64 Linux runners are not offered on every GitHub plan. A release
missing it says so at warning level rather than shipping quietly incomplete. See
[docs/releasing.md](docs/releasing.md).

## Layout

```
mitosync/
├── include/          # public headers, one per translation unit in src/
├── src/              # implementation
│   ├── main.cpp                  # CLI dispatch and orchestration
│   ├── crc32_hw.cpp              # CRC32 via zlib-ng + runtime CPU reporting
│   ├── crc32_chunks.cpp          # parallel chunked checksums over pread
│   ├── directory_comparison.cpp  # discovery + adaptive-concurrency comparison
│   ├── comparison_task.cpp       # per-file chunk comparison
│   ├── sync_task.cpp             # sync, including differential transfer
│   ├── rm_task.cpp               # S3 delete
│   ├── leftovers_task.cpp        # incomplete multipart uploads
│   ├── s3_utils.cpp              # S3MultipartCopy, object metadata
│   ├── cloud_metrics.cpp         # API call accounting
│   ├── aws_pricing.cpp           # cost estimation
│   └── report_writer.cpp         # JSON / CSV / text reports
├── tests/            # GoogleTest suite
├── book/             # mdBook documentation source
├── docs/             # design notes and architecture write-ups
└── scripts/          # coverage, packaging and release checks
```

## Security and privacy

`mito` holds AWS credentials and can delete objects. Both deserve a plain explanation.

**It reads credentials and stores none.** The standard AWS chain only: environment variables,
`~/.aws/credentials` and `~/.aws/config` including named profiles, or an ECS task role. Nothing is
written back to any of them, and no credential reaches a report or the metrics file.

**The state it keeps names no buckets and no keys.** `cloud_metrics.json` holds call counts, byte
totals, latencies and the regions seen. `pricing_cache.json` holds the built-in S3 price table. Both
are described in [Files on disk](https://echemythia.github.io/mitosync/reference/files.html).

**Reports are yours to protect.** A report written with `-o` names local paths, bucket names and
object keys. It is created with an ordinary open, so your `umask` decides its mode, an existing file
is truncated, and a symlink is followed. Run `umask 077` first if that matters, and do not point
`-o` at a directory someone else can write to.

**Deletion is gated twice.** `rm` deletes nothing without `--force`, and refuses a prefix without
`--recursive`.

**A partial listing stops the run.** If a side cannot be listed completely, `sync` plans nothing and
`diff` reports nothing, for the reason given under [Sync](#sync).

**Comparison creates scratch multipart uploads and always aborts them**, on every path out
including the error paths. A hard kill can still leave parts behind, which S3 keeps billing for and
which appear in no object listing. `mito leftovers` finds them.

**Object ownership after an S3-to-S3 copy is S3's rule, not ours.** The copy is written by the
destination principal and owned by that account. `mito` sets no ACLs. See
[Cross-account S3](https://echemythia.github.io/mitosync/reference/cross-account.html).

To report a security problem, see [SECURITY.md](SECURITY.md).

## Disclaimer

MitoSync is a personal project, written to learn hardware checksums, C++ concurrency and the S3
API. It is provided for educational and experimental purposes only, with no warranty of any kind.
It is not intended or recommended for use in production or business-critical systems. Evaluate it
carefully and run it at your own risk on any system or bucket you rely on.

## License

Apache-2.0. See [`LICENSE`](LICENSE).

Published artifacts include [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md), which lists the
third-party code redistributed in release binaries and reproduces the relevant license texts.
GoogleTest is used for tests only and is not redistributed.
