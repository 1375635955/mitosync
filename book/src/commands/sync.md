# `sync`: make one side match

```
mito sync <source> <destination> [OPTIONS]
```

`sync` makes the destination look like the source. It works between a local folder and S3, in
either direction, or between two S3 prefixes.

Direction comes from the order you type things. **Source first, destination second.**

## Options

| Flag | Meaning |
|------|---------|
| `--delete` | Remove files at the destination that are not at the source |
| `--dry-run` | Print what would happen and change nothing |
| `-t`, `--threads <N>` | Maximum concurrent operations (default 256) |
| `--source-profile <name>` | AWS profile for the S3 source (S3 to S3, or download) |
| `--dest-profile <name>` | AWS profile for the S3 destination (S3 to S3, or upload) |
| `--endpoint-url <url>` | S3-compatible endpoint |
| `--allow-unverified-ranges` | Accept a ranged read whose response has no `Content-Range`. Weakens a safety check, see [S3-compatible storage](../reference/s3-compatibility.md#ranged-reads-must-carry-content-range) |
| `-q`, `--quiet` | Minimal output |
| `-v`, `--verbose` | Detailed progress |
| `-d`, `--debug` | Debug logging |
| `-h`, `--help` | Help |

## Examples

```bash
mito sync ./data/ s3://my-bucket/backup/            # upload
mito sync s3://my-bucket/backup/ ./data/            # download
mito sync s3://my-bucket/a/ s3://my-bucket/b/       # S3 to S3, copied inside AWS
mito sync ./data/ s3://my-bucket/backup/ --delete   # upload, prune extras
mito sync ./data/ s3://my-bucket/backup/ --dry-run  # preview only
```

## How it decides what to copy

This is the most important thing to understand about `sync`, so here it is on its own line:

> **A file is skipped only when the sizes match *and* the destination is newer than the
> source. Everything else gets transferred.**

```
   for each file:

   size different?  ──── yes ──►  TRANSFER
          │
          no
          ▼
   destination newer
   than source?     ──── no  ──►  TRANSFER
          │
          yes
          ▼
        SKIP
```

Notice what is missing from that diagram: any checksum. Both signals, size and timestamp, come
out of the folder listing each side already produced. So `sync` can plan a run over a huge tree
without reading a single byte of file content. That is what makes it fast.

Size alone would not be enough. Plenty of edits do not change a file's length: a fixed-width
record overwritten in place, a binary patched byte for byte, content swapped for different
content of the same size. In all of those the size is identical and only the timestamp moves.

### Why "newer" and not "not older"

The check is **strictly newer**. Here is why that matters.

Both timestamps are whole seconds. If you edit a file in the same second that the destination
was written, the two stamps come out equal. Neither one will ever change again on its own. So if
"equal" counted as a skip, that file would be skipped forever.

Requiring the destination to be *strictly* newer costs you at most one redundant transfer. After
that the destination's timestamp moves ahead and the file settles down into being skipped.

### An unknown timestamp means transfer

If either side reports no timestamp, `sync` transfers. A zero proves nothing. Copying something
you did not need to costs bandwidth; skipping something you needed leaves stale data. The first
mistake is cheaper.

### What this still gets wrong

Two clocks are being compared, with no tolerance for skew. So:

- **A restore that puts an old timestamp back gets skipped.** `tar -x`, `cp -p` and `rsync -t`
  all set the extracted file's mtime to the original's. That is older than the destination
  written after it, so the destination looks current and your restore is silently not
  propagated.
- **A wrong local clock either hides edits or re-copies everything**, depending on which way it
  is wrong.

When being correct matters more than being fast, run [`diff`](diff.md). It checksums.

## A partial listing stops the run

If either side cannot be listed completely, `sync` **refuses to do anything at all**.

That is stricter than it sounds, and it is deliberate. A half-listed folder does not look
half-listed. It looks small. Without `--delete`, `mito` would copy part of your source and
report success. With `--delete`, every file the failed listing forgot to mention looks exactly
like a file that should be removed from the destination.

So the run stops with an error. The paths it could not read are logged just above it.

## Differential transfer

Once a file is picked for transfer, its size decides *how* it moves.

```
   file < 8 MiB          send the whole thing

   file >= 8 MiB         compare chunk checksums first,
                         send only the chunks that differ

                         [c0][c1][c2][c3][c4]   source
                          ✓   ✓   X   ✓   X     compare
                                  │       │
                                  ▼       ▼
                              send only these two
```

For a large file with a small change, this is the difference between moving gigabytes and moving
megabytes.

Differential transfer uses the same remote-checksum trick as `diff`. On a service that does not
implement S3 additional checksums, it fails for files above the chunk size. See
[S3-compatible storage](../reference/s3-compatibility.md).

## `--delete`

`--delete` removes anything at the destination that has no counterpart at the source.

Combined with a mistyped source path, it will empty a prefix. Dry-run first. Every time.

```bash
mito sync ./data/ s3://my-bucket/backup/ --delete --dry-run
```

`--delete` is also the reason a partial source listing is refused instead of tolerated. See
[above](#a-partial-listing-stops-the-run).

## S3 to S3

An S3-to-S3 sync is a **server-side copy**. The bytes move inside AWS and never travel through
your machine. That is faster and avoids egress charges.

There is a catch that surprises people, and it is worth understanding before you try it across
accounts:

```
   Your machine
        │
        │ 1. list source      (uses --source-profile)
        │ 2. list destination (uses --dest-profile)
        │ 3. "copy A to B"    (uses --dest-profile)
        │        │
        ▼        │
   ┌─────────────┼──────────────┐
   │  AWS        ▼              │
   │   source ──────► dest      │   bytes move in here,
   │                            │   under the DESTINATION's
   └────────────────────────────┘   credentials
```

The copy is one API call, sent to the destination, naming the source in a header. There is only
one set of credentials on that request, and they belong to the **destination** profile. So the
destination principal has to be allowed to read the source bucket.

`--source-profile` authenticates the source *listing*. It cannot authenticate the copy.

A real cross-account copy therefore also needs a policy on the source bucket. See
[Cross-account S3](../reference/cross-account.md).

## Exit status

| Code | Means |
|------|-------|
| `0` | Success |
| `1` | Something failed |
| `2` | Bad command line, or a local path that is not a folder |
| `130` | You pressed Ctrl-C |

An interrupted `sync` leaves the destination partly updated. There is no rollback. Running the
same command again converges. See [Exit codes](../reference/exit-codes.md).
