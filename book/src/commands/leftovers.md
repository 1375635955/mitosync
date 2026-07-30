# `leftovers`: incomplete uploads

```
mito leftovers <bucket> [OPTIONS]
```

`leftovers` finds multipart uploads that were started and never finished, and can clean them up.

## The problem it solves

Here is a story that happens all the time.

You start uploading a 200 GB file. S3 splits it into parts. At 180 GB, the network drops.

Those 180 GB of parts are still sitting in your bucket. S3 is still charging you for them. And
here is the part that catches people out:

```
   ListObjectsV2 on the bucket   ──►  nothing
   the AWS console               ──►  nothing
   your monthly bill             ──►  180 GB
```

Incomplete uploads appear in **no object listing**. They are invisible to every normal way of
looking at a bucket. The only place they show up is the bill.

So every failed upload, every cancelled CI job that was pushing an artifact, every laptop that
went to sleep mid-transfer, leaves a silent recurring charge behind. This is one of the most
common reasons an S3 bill drifts away from what the object listing says it should be.

`mito`'s own comparison path creates scratch multipart uploads and always aborts them. But any
client can leave leftovers, and one badly timed network failure is enough.

## Options

| Flag | Meaning |
|------|---------|
| `--prefix <path>` | Only uploads under this prefix |
| `--older-than <dur>` | Only uploads older than a duration, e.g. `1h`, `1d`, `7d` |
| `--abort` | Abort every matching upload. Without it, the command only lists |
| `--region <region>` | Pin the region instead of detecting it |
| `--endpoint-url <url>` | S3-compatible endpoint |
| `-v`, `--verbose` | Detailed output |
| `-h`, `--help` | Help |

The bucket can be given bare or as a URL. Both `my-bucket` and `s3://my-bucket` work.

## Examples

```bash
# What is in there?
mito leftovers my-bucket

# Only the stale ones, under one prefix
mito leftovers my-bucket --prefix uploads/ --older-than 7d

# Clean them up
mito leftovers my-bucket --older-than 7d --abort
```

The safe habit is the same as with `rm`: list first with `--older-than`, read the output, then
add `--abort` to the identical command.

**Use `--older-than` even when cleaning.** It protects uploads that are legitimately in flight
right now. A blanket `--abort` would kill somebody's running transfer.

## Duration format

A number and a unit: `30m`, `1h`, `1d`, `7d`. Anything it cannot parse is a usage error and
exits `2`.

## A permanent fix instead

This command is for finding out what is already there, and for buckets whose configuration you
do not control.

If you do control the bucket, set a lifecycle rule with `AbortIncompleteMultipartUpload` on it.
Then S3 cleans up after itself and you never have to think about this again.

## Exit status

| Code | Means |
|------|-------|
| `0` | Success |
| `1` | One or more aborts failed |
| `2` | No bucket, a bad duration, an unknown option, or a region that could not be detected |

See [Exit codes](../reference/exit-codes.md).
