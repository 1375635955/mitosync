# Security and privacy

`mito` holds AWS credentials and can delete objects. Both of those deserve a plain explanation.

## Credentials

`mito` reads the standard AWS chain and **writes nothing back to it**: environment variables,
`~/.aws/credentials` and `~/.aws/config` including named profiles, or an ECS task role.

It stores no credential of its own. None reaches a report, a log line, or the metrics file.

Per-side profiles (`--source-profile`, `--dest-profile`) pick among those same named profiles. See
[Cross-account S3](cross-account.md).

> **Instance metadata lookup is disabled at startup.** `mito` sets
> `AWS_EC2_METADATA_DISABLED=true` unconditionally, overwriting any value you set.
>
> Off an EC2 instance, that removes a lookup that can only time out. On one, it removes
> instance-role credentials from the chain entirely, so a run relying on them fails to
> authenticate. Use a profile, environment variables, or an ECS task role there.

## What it writes

Two small state files, neither of which names a bucket or an object:

- `cloud_metrics.json` — per-operation call counts, byte totals, latencies, and the regions seen
- `pricing_cache.json` — the built-in S3 price table used for cost estimates, keyed by region

Paths are in [Files on disk](files.md).

### Reports are the sensitive artefact

A report written with `-o` names local paths, bucket names and object keys. That is the file worth
thinking about.

It is created with an ordinary open, which means:

- **your `umask` decides its permissions.** Run `umask 077` first if that matters.
- **an existing file at that path is truncated.**
- **a symlink at that path is followed.**

So treat `-o` the way you would treat any shell redirection. Do not point it at a directory
another user can write to.

## Destructive operations

Three independent gates stand between a typo and data loss.

```
   1. rm without --force        ──►  preview only, nothing deleted

   2. rm on a prefix without -r ──►  error, not a mass delete

   3. a listing that failed     ──►  the whole run stops
                                     nothing planned, nothing deleted
```

The third one deserves the most explanation, because it is the least obvious.

If either side could not be enumerated completely, `sync` plans nothing and deletes nothing, and
`diff` reports nothing. A partial listing is indistinguishable from a smaller tree. Under
`--delete`, every file the failed listing forgot to mention looks exactly like a destination
orphan to be removed. In a comparison, those same files look exactly like files present on only
one side.

See [`sync`](../commands/sync.md#a-partial-listing-stops-the-run) and
[`diff`](../commands/diff.md#a-partial-listing-stops-the-run).

`--dry-run` on `sync`, and the no-`--force` preview on `rm`, are the intended way to check a
command before it does anything.

## Scratch state in S3

Comparing a remote object creates a scratch multipart upload and aborts it on every path out,
including the error paths.

A process killed hard enough to skip that abort leaves parts behind. S3 keeps billing for them,
and they appear in no object listing.
[`mito leftovers`](../commands/leftovers.md) finds and clears them.

## Object ownership

An object written by an S3-to-S3 copy is owned by the account that wrote it, which is the
**destination** principal.

Depending on the destination bucket's Object Ownership setting, that can leave objects the bucket
owner cannot read. `mito` sets no ACLs and does not manage ownership. See
[Cross-account S3](cross-account.md#object-ownership-after-a-copy).

## Transport

All S3 traffic goes through the AWS SDK for C++ over TLS, using OpenSSL as linked by vcpkg.

`mito` does not disable certificate verification and offers no flag to do so.

`--endpoint-url` changes the host and switches to path-style addressing. It does not relax any of
this.
