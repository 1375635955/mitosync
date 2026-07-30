# Exit codes

| Code | Meaning |
|------|---------|
| `0` | Success. For `diff`, everything matched |
| `1` | A mismatch was found, or the operation failed |
| `2` | Bad command line or configuration |
| `130` | Interrupted by Ctrl-C (128 + 2) |

```
   0  ─── it worked
   1  ─── it ran, and the answer was bad OR it broke partway
   2  ─── it never started. your command line was wrong.
  130  ─── you stopped it
```

## `0`

The command did what you asked.

For `diff` this also means every single file matched, so this reads correctly:

```bash
mito diff a/ b/ && echo "identical"
```

## `1`

Two quite different situations share this code:

- `diff` found a real difference
- something failed

That is a genuine limitation, and it is worth stating plainly rather than hiding.

**If your script needs to tell those apart, do not read the exit code.** Write a report and read
the counters, where the two are separate:

```bash
mito diff ./data/ s3://my-bucket/data/ -o results.json
if [ $? -ne 0 ]; then
  jq '.summary | {different, errors}' results.json
fi
```

`summary.different` counts genuine mismatches. `summary.errors` counts files that could not be
compared. See the [`diff` report format](../commands/diff.md#report-formats).

`leftovers` also returns `1`, when one or more aborts failed.

## `2`

Something was wrong with the command line or the configuration. Every command can return this.
Concretely:

- an unknown option
- missing arguments: no S3 URL or bucket, or fewer than two sources for `diff`
- a prefix passed to `rm` without `--recursive`
- a local path given to `sync` that is not a directory
- an `--older-than` duration that could not be parsed
- a bucket region that could not be detected

**The defining feature of a `2` is that nothing was attempted.** Running the same command again
will fail exactly the same way, so a retry loop should treat it as terminal and give up.

## `130`

Ctrl-C, following the usual `128 + signal` convention.

Cancellation is cooperative, so the process finishes the unit of work it is in (one chunk
checksum, one S3 request) before exiting. It is not instant. That is deliberate: a half-written
destination file is worse than a second of delay.

**An interrupted `sync` leaves the destination partly updated.** There is no transaction and no
rollback.

Running the same `sync` again converges, though. Anything already transferred now matches by size
and carries a destination timestamp newer than its source, so it gets skipped on the second pass.
Anything caught mid-transfer does not match, and gets redone.

## Scripting notes

Gate a deploy on the upload having landed intact:

```bash
if ! mito diff ./build/ s3://artifacts/build/ -q; then
  echo "artifact mismatch, refusing to promote" >&2
  exit 1
fi
```

Nightly cleanup that does not retry a configuration mistake:

```bash
mito leftovers my-bucket --older-than 7d --abort || rc=$?
[ "${rc:-0}" -eq 2 ] && echo "config problem, not retrying" >&2
```

One gotcha under `set -e`: `mito diff` exiting `1` on a legitimate mismatch will abort your
script. Usually that is exactly what you want. When it is not, use `|| true` or wrap it in an
explicit `if`.
