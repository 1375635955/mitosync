# `diff`: compare by checksum

```
mito [diff] <source1> <source2> [OPTIONS]
```

`diff` answers one question: **are these two things the same?**

It answers it by checksum, not by size or timestamp. If it says two files match, it read every
byte of both, one way or another. This is the command you run when you need to be sure.

It is also the default command, so `mito a/ b/` and `mito diff a/ b/` mean the same thing.
Either side can be local or S3, in any combination.

## Options

| Flag | Meaning |
|------|---------|
| `-D`, `--directory` | Force directory comparison mode |
| `-o`, `--output <file>` | Write a report. Format follows the extension: `.json`, `.csv`, `.txt` |
| `--source-profile <name>` | AWS profile for source1, if it is S3 |
| `--dest-profile <name>` | AWS profile for source2, if it is S3 |
| `--endpoint-url <url>` | S3-compatible endpoint. Also turns on path-style addressing |
| `--allow-unverified-ranges` | Accept a ranged read whose response has no `Content-Range`. Weakens a safety check, see [S3-compatible storage](../reference/s3-compatibility.md#ranged-reads-must-carry-content-range) |
| `-t`, `--threads <N>` | Thread budget (default 1024). Actual concurrency adapts below this |
| `-r`, `--ramp-up` | Widen concurrency gradually. Helps when DNS is the slow part |
| `-P`, `--parallel-discovery` | Parallel folder scanning (on by default) |
| `--no-parallel-discovery` | Scan folders one at a time instead |
| `--parallel-discovery-workers <N>` | Scanning workers, 1 to 128 (default 128) |
| `-q`, `--quiet` | Errors and summary only |
| `-v`, `--verbose` | Show retry warnings, which are hidden by default |
| `-d`, `--debug` | Debug logging |
| `-h`, `--help` | Help |
| `-V`, `--version` | Print the version and exit |

## Examples

```bash
# Two local folders
mito /path/to/dir1/ /path/to/dir2/

# One local file against one S3 object
mito ./file.bin s3://my-bucket/file.bin

# Local tree against an S3 prefix, with a report a script can read
mito diff ./data/ s3://my-bucket/data/ -o results.json

# Two buckets in different accounts and regions
mito diff s3://acctA-bucket/data/@us-east-1 s3://acctB-bucket/data/@eu-west-1 \
  --source-profile acctA --dest-profile acctB
```

## What actually happens

Every file is cut into 8 MiB chunks. Each chunk gets its own checksum. Then the two lists of
checksums are compared.

The two sides get there differently:

```
   LOCAL SIDE                          S3 SIDE

   mmap the chunk                      ask S3 to checksum
   (no copy, pages in                  that byte range
    only what is read)                 (bytes never move)
          │                                   │
          ▼                                   ▼
   zlib-ng CRC32                       read the answer from
   using your CPU's                    the x-amz-checksum-crc32
   CRC instructions                    response header
          │                                   │
          ▼                                   ▼
   [c0][c1][c2][c3]  ◄── compare ──►   [c0][c1][c2][c3]
```

Both sides run at the same time. So the whole thing takes as long as the *slower* of the two,
not both added together. That matters, because they are limited by completely different things:
your disk and CPU on one side, network round trips on the other.

## Finding where two files differ

Knowing "chunk 47 is wrong" is not very useful. An 8 MiB chunk is a big haystack.

So when a chunk disagrees, `diff` goes back and re-checks just that chunk at 64 KiB
granularity, 128 blocks of it:

```
  Pass 1, coarse:  [ ][ ][ ][X][ ][ ][ ][ ]   ← 8 MiB chunks, cheap
                            │
                            ▼
  Pass 2, fine:    [ ][ ][X][ ] ... (128 blocks of 64 KiB)
                          │
                          ▼
              "bytes 25165824-25231359 differ"
```

Why two passes instead of just checking everything at 64 KiB? Because that would multiply the
number of S3 calls by 128, on a comparison that usually finds nothing at all. Doing the fine
pass only where the coarse pass found trouble keeps the normal case cheap.

## A partial listing stops the run

If `mito` cannot read one side completely, `diff` fails. It does not report on the part it
managed to read.

This sounds unhelpful until you think about what the alternative would print. A folder that was
only half-listed does not look half-listed. It looks *small*. Every file it failed to mention
would be reported as `ONLY <other source>`, which turns a typo or a permissions problem into a
confident, wrong answer.

That matters more here than almost anywhere else, because `diff` is the command people run to
decide whether it is safe to delete the other copy.

The error names which side was incomplete. The specific paths it could not read are logged just
above it. [`sync`](sync.md#a-partial-listing-stops-the-run) refuses for the same reason.

## Report formats

The extension you give to `-o` decides the format. Anything that is not `.json` or `.csv` comes
out as text.

All three list **only what is wrong**: files that differ, files present on one side only, and
files that could not be read. Matching files are left out. That is deliberate. It is what keeps
a report over a million matching objects small enough to read.

### JSON

A summary, then an array of problems.

```json
{
  "source_a": "./data/",
  "source_b": "s3://my-bucket/data/",
  "summary": {
    "total_files": 1204,
    "matching": 1201,
    "different": 2,
    "only_in_./data/": 1,
    "only_in_s3://my-bucket/data/": 0,
    "errors": 0,
    "elapsed_seconds": 14.30
  },
  "files": [
    { "path": "logs/app.bin", "status": "MISMATCH", "size_a": 8388608, "size_b": 8388608 }
  ]
}
```

`different` and `errors` are separate counters. That is how a script tells "these two copies
genuinely differ" from "I could not read one of them". The exit code cannot tell you this, so
if you care, read the report. See [Exit codes](../reference/exit-codes.md).

Two things to know before you parse this:

- **The `only_in_*` key names change every run.** They are built from the source strings you
  typed. Read them by position or by prefix, never by literal name.
- **Strings are not escaped.** A path or error message containing a `"` or a `\` will produce
  JSON that does not parse. Ordinary paths are fine. Unusual ones are worth knowing about
  before a nightly job trips over one.

### CSV

A header row, then one row per problem file.

```csv
status,path,size_a,size_b,error
MISMATCH,"logs/app.bin",8388608,8388608,""
ONLY ./data/,"logs/new.bin",4096,-1,""
```

A size of `-1` means the file is not there on that side. There is no summary block. The counts
only appear in the JSON and text formats. The same escaping caveat applies.

### Text

The default for any other extension. Written for a person to read.

```text
Directory Comparison Results
============================

Source A: ./data/
Source B: s3://my-bucket/data/

Summary:
  Total files:      1204
  Matching:         1201
  Different:        2
  Only in ./data/: 1
  Only in s3://my-bucket/data/: 0
  Errors:           0
  Time:             14.30s

Differences:
  [MISMATCH] logs/app.bin
  [ONLY ./data/] logs/new.bin
```

The `Differences:` section disappears entirely when there is nothing to report.

## Exit status

| Code | Means |
|------|-------|
| `0` | Everything matched |
| `1` | Something differed, **or** something failed |
| `2` | Bad command line (unknown option, fewer than two sources) |
| `130` | You pressed Ctrl-C |

Code `1` covers both "they differ" and "it broke". If your script needs to tell those apart,
write a report and read the counters. See [Exit codes](../reference/exit-codes.md).

## Tuning

The defaults are meant to be fine. If you are tuning anyway:

- **Lots of small files** are limited by folder scanning. Give them a wide comparison window.
- **A few huge files** are limited by chunk work. Give them more threads per file instead.
- `--ramp-up` helps when a cold DNS cache is what is really slowing down the start.

Most of this is worked out automatically by measuring throughput as it goes. See
[Parallelism](../reference/parallelism.md).
