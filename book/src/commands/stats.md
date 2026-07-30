# `stats`: API usage and cost

```
mito stats [OPTIONS]
```

`stats` tells you what `mito` has been doing to your AWS bill.

Every command records the S3 calls it makes. `stats` prints the running totals and estimates
what they cost.

## Options

| Flag | Meaning |
|------|---------|
| `--json` | Machine-readable output |
| `--reset` | Clear the counters and start again |
| `-h`, `--help` | Help |

## The counts add up over time

The counters are **cumulative** and live on disk. So `mito stats` answers "what has this tool
cost me so far", not "what did the last command cost".

If you want to measure one specific job, reset first:

```bash
mito stats --reset
mito diff ./data/ s3://my-bucket/data/
mito stats
```

For each kind of operation it tracks calls, successes, failures, retries, bytes up, bytes down,
bytes moved server-side, and latency (average and worst).

## Why the operation types look oddly specific

The types are split more finely than the S3 API names are, because the **billing** is split more
finely:

| Type | Why it is separate |
|------|--------------------|
| `HeadObject`, `GetObject` | GET-class pricing |
| `PutObject`, `ListObjectsV2`, `ListBuckets` | PUT-class pricing, roughly 12x the GET price |
| `DeleteObject`, `DeleteObjects` | Free on AWS S3 |
| `UploadPartCopy`, `CopyObject` | Copy inside one bucket. No data transfer charge |
| `UploadPartCopyRemote`, `CopyObjectRemote` | Copy between buckets. May cross regions, may cost transfer |
| `CreateMultipartUpload`, `UploadPart`, `CompleteMultipartUpload`, `AbortMultipartUpload` | Multipart lifecycle |
| `ListParts`, `ListMultipartUploads`, `GetBucketLocation` | Metadata calls |

The same-bucket versus cross-bucket split is decided by comparing the source bucket named in the
copy header against the destination bucket. It matters because a same-bucket copy is priced as a
request, while a cross-region one also moves data.

## Where the prices come from

**Built-in per-region defaults, compiled into the binary.** Nothing is fetched.

```
   built-in per-region defaults  ──► cached on disk ──► estimate
```

Fetching live rates from the AWS Pricing API is not implemented
([#106](https://github.com/echemythia/mitosync/issues/106)). There is a cache file with a
24-hour lifetime, but what it caches is the same built-in table, so it changes nothing today.

The practical consequence: the rates are a snapshot someone typed in, not what AWS is charging
you this month. They will drift.

```bash
mito stats --json
```

```json
{
  "total": {
    "calls": 0,
    "success": 0,
    "failures": 0,
    "retries": 0,
    "bytes_uploaded": 0,
    "bytes_downloaded": 0,
    "avg_latency_ms": 0.00
  },
  "operations": {},
  "cost_breakdown": {
    "requests_usd": 0.000000,
    "data_transfer_out_usd": 0.000000,
    "total_usd": 0.000000
  },
  "region": "us-east-1"
}
```

## Treat the number as an estimate, not a bill

It only covers requests `mito` itself made. It knows nothing about:

- storage costs
- your negotiated pricing
- free-tier allowances
- traffic from anything else on the account

Its real value is **comparative**. Run a workload two ways and see which one made an order of
magnitude fewer PUT-class calls. That comparison is reliable even when the dollar figure is not.

If you point `mito` at a non-AWS service, the call counts stay accurate and the dollars become
meaningless, because everything is priced at AWS rates. See
[S3-compatible storage](../reference/s3-compatibility.md#cost-accounting-on-non-aws-endpoints).

## Where the state lives

See [Files on disk](../reference/files.md).

`--reset` does not delete the metrics file. It clears the counters and writes an empty file over
the top. If that write fails, the command says so and exits `1`, rather than claiming a reset
that did not happen. It leaves the pricing cache alone.
