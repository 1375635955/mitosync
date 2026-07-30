# Files on disk

`mito` keeps two small pieces of state outside your project. Neither one holds credentials, and
neither names a bucket or an object.

[Security and privacy](security.md) covers the wider picture.

```
   ~/.local/share/mitosync/cloud_metrics.json   API call counters   (Linux)
   ~/Library/Application Support/MitoSync/      same               (macOS)

   ~/.mitosync/pricing_cache.json               S3 price table     (both)

   ./results.json                               only when you ask with -o
```

## Metrics

The cumulative API counters behind [`mito stats`](../commands/stats.md), stored as
`cloud_metrics.json`:

| Platform | Path |
|----------|------|
| Linux | `$XDG_DATA_HOME/mitosync/`, or `~/.local/share/mitosync/` |
| macOS | `~/Library/Application Support/MitoSync/` |

The directory is created on first use, recursively, because `~/.local/share` does not exist on a
minimal install.

Deleting the file resets the counters.

`mito stats --reset` does **not** delete it. It clears the counters in memory and writes an empty
metrics file over the top. If that write fails, the command reports the failure and exits `1`
rather than claiming a reset that never happened.

## Pricing cache

The S3 price table used for cost estimates, cached as `pricing_cache.json` in `~/.mitosync/`,
keyed by region with a 24-hour lifetime.

**The prices are built-in per-region defaults compiled into the binary, not fetched from AWS.**
Live lookup against the AWS Pricing API is not implemented
([#106](https://github.com/echemythia/mitosync/issues/106)), so the cache currently stores the
same table the binary already contains and the file earns nothing. It exists for the day the
lookup lands.

Deleting the file is harmless.

Note that the two files live in **different** directories. The pricing cache uses `~/.mitosync/`
on every platform, while metrics follow the platform convention. That is a wrinkle, not a design
statement.

## AWS credentials

`mito` reads the standard AWS chain and **writes nothing to it**:

- `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_SESSION_TOKEN`
- `~/.aws/credentials` and `~/.aws/config`, including named profiles
- An ECS task role

Named profiles are selected per S3 side with `--source-profile` and `--dest-profile`. See
[Cross-account S3](cross-account.md).

> **EC2 instance roles do not work.**
>
> `mito` sets `AWS_EC2_METADATA_DISABLED=true` at startup, unconditionally, overwriting any value
> you set.
>
> The reason: off an EC2 instance, an instance-metadata lookup can only ever time out, and it adds
> that timeout to every cold start. Disabling it removes the delay.
>
> The cost: on an actual EC2 instance, it also removes instance-role credentials from the chain.
> A run that relies on them will fail to authenticate. Use environment variables, a profile, or an
> ECS task role there.

## What gets written where you run it

Report files, and only when you ask for one:

```bash
mito diff ./data/ s3://my-bucket/data/ -o results.json
```

Nothing else is written to the working directory.

There is one exception, as a fallback. If the application data directory cannot be determined
**and** cannot be created, metrics land in the current directory instead.

## Scratch state in S3

Comparing a remote object creates a scratch multipart upload, and aborts it on every path out
including the error paths. See [Architecture](architecture.md).

If a process is killed hard enough to skip that abort, the parts are left behind and S3 keeps
billing for them. [`mito leftovers`](../commands/leftovers.md) is how you find and clear those.
