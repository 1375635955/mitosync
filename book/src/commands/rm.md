# `rm`: delete S3 objects

```
mito rm <s3-url> [OPTIONS]
```

`rm` deletes one S3 object, or everything under a prefix.

It is the only command here that destroys data on purpose, so it is built to be hard to trigger
by accident.

## Options

| Flag | Meaning |
|------|---------|
| `-r`, `--recursive` | Delete everything under the prefix |
| `-f`, `--force` | Actually delete. Required |
| `--batch` | Use the batch `DeleteObjects` API. Faster, more likely to get throttled |
| `-t`, `--threads <N>` | Maximum concurrent deletions (default 256) |
| `--endpoint-url <url>` | S3-compatible endpoint |
| `-q`, `--quiet` | Minimal output |
| `-v`, `--verbose` | Print each deleted object |
| `-h`, `--help` | Help |

## Two locks on the door

You have to get past both of these before anything is deleted.

```
   mito rm s3://bucket/logs/
        │
        ▼
   is it a prefix, with no -r ?  ──► yes ──►  ERROR, exit 2
        │                                     (not a no-op,
        no                                     not a mass delete)
        ▼
   was -f given ?                ──► no  ──►  PREVIEW only,
        │                                     nothing deleted
        yes
        ▼
   delete
```

The first lock means a prefix always needs `--recursive`. Pointing `rm` at
`s3://bucket/logs/` without it is an error. It does not quietly do nothing, and it certainly
does not delete everything.

The second means nothing is deleted without `--force`. Leave it off and you get a preview of
exactly what would go.

```bash
mito rm s3://my-bucket/old-data/            # error: prefix needs -r
mito rm s3://my-bucket/old-data/ -r         # preview of the recursive delete
mito rm s3://my-bucket/old-data/ -rf        # do it
mito rm s3://my-bucket/file.txt --force     # a single object
```

The habit worth building: run it once without `-f`, read the list, then add `-f` to the same
command.

## `--batch`

By default each object is deleted with its own request, `-t` of them at a time.

`--batch` switches to the `DeleteObjects` API, which removes up to 1000 keys in a single
request. Over a large prefix this is much faster. It is also much more likely to draw throttling
responses from S3.

Use it when you are deleting a lot and you are not competing with production traffic.

On AWS S3, DELETE requests are free either way. So this choice is about speed and throttling,
not cost.

## Exit status

| Code | Means |
|------|-------|
| `0` | Success |
| `1` | The delete failed |
| `2` | No S3 URL, a prefix without `-r`, or a region that could not be detected |
| `130` | You pressed Ctrl-C |

See [Exit codes](../reference/exit-codes.md).
