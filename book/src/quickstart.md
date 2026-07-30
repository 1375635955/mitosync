# Quickstart

Five minutes. You need a working `mito` and AWS credentials in the usual place.

## First, how to name things

Every command takes paths in the same four shapes. Learn these and the rest is easy.

```
  /home/me/report.pdf        one local file
  /home/me/data/             a local folder  (note the slash)
  s3://my-bucket/report.pdf  one S3 object
  s3://my-bucket/data/       an S3 folder    (note the slash)
```

**The trailing slash matters.** It is the only thing that tells `mito` whether you mean one
file or a whole tree.

You can also pin a region by adding `@region` to any S3 path:

```bash
s3://my-bucket/data/@eu-west-1
```

Without it, `mito` asks S3 where the bucket lives. That costs one extra API call, and it fails
on some non-AWS services. Pinning is faster and more portable, so it is a good habit.

## Compare two local folders

```bash
mito ./data/ /mnt/backup/data/
```

`diff` is the default command, so you can leave the word out. Exit code `0` means everything
matched.

## Compare local against S3

```bash
mito diff ./data/ s3://my-bucket/data/
```

Nothing big gets downloaded. That is the whole point. See
[Architecture](reference/architecture.md).

## Get a report a script can read

```bash
mito diff ./data/ s3://my-bucket/data/ -o results.json
```

The file extension picks the format: `.json`, `.csv`, or plain text for anything else.

When a file differs, the report does not just say "this file is wrong". It narrows the problem
down for you:

```
  file.bin  ──►  8 MiB chunks  ──►  the one that differs  ──►  64 KiB blocks
                 [ ][ ][X][ ]              chunk 2            [ ][X][ ][ ]

  result: "bytes 16777216-16842751 differ"
```

So you learn *where* two copies diverged, not just *that* they did.

## Sync, carefully

Always dry-run first. `--delete` really does delete.

```bash
mito sync ./data/ s3://my-bucket/backup/ --dry-run   # look
mito sync ./data/ s3://my-bucket/backup/             # do it
```

Direction comes from the order of the arguments. Source first, destination second. Swap them to
download instead of upload.

Remember: `sync` decides by **size and modification time**, not by checksum. It skips a file
only when the sizes match and the destination is newer. That catches an ordinary edit. It does
not catch a restore that put an old timestamp back. When you need certainty, run `diff`.

## Delete objects

`rm` will not delete anything until you add `--force`. Without it, you get a preview.

```bash
mito rm s3://my-bucket/old-data/ --recursive          # preview
mito rm s3://my-bucket/old-data/ --recursive --force  # do it
```

## See what it cost

```bash
mito stats
```

The counts add up across every run and are kept on disk. See [stats](commands/stats.md).

## Clean up what failed

```bash
mito leftovers my-bucket --older-than 7d           # list
mito leftovers my-bucket --older-than 7d --abort   # clean
```

When a big upload fails halfway, S3 keeps the parts it already received. It also keeps charging
you for them. They show up in no object listing, so nobody ever notices. This is how you find
them.

## Where to go next

- [diff](commands/diff.md) and [sync](commands/sync.md) for the full flag lists
- [Exit codes](reference/exit-codes.md) if you are scripting this
- [S3-compatible storage](reference/s3-compatibility.md) if you are not using AWS
