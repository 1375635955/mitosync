# Cross-account S3

`sync` and `diff` let each S3 side authenticate as a different AWS profile, using
`--source-profile` and `--dest-profile`. Leave one off to use the default credential chain.

| Flag | `sync` | `diff` |
|------|--------|--------|
| `--source-profile <name>` | S3 source (S3 to S3, or download) | source1, if it is S3 |
| `--dest-profile <name>` | S3 destination (S3 to S3, or upload) | source2, if it is S3 |

These are ordinary named profiles from `~/.aws/credentials` and `~/.aws/config`, including ones
that assume a role.

```bash
# Compare two buckets in two accounts
mito diff s3://acctA-bucket/data/ s3://acctB-bucket/data/ \
  --source-profile acctA --dest-profile acctB

# Copy between them
mito sync s3://acctA-bucket/data/ s3://acctB-bucket/data/ \
  --source-profile acctA --dest-profile acctB
```

## Two things will bite you, in this order

### 1. Region detection needs `GetBucketLocation`

Auto-detection calls `GetBucketLocation` on each bucket. Cross-account, that call is very often
denied even when object access is granted, because bucket-level and object-level permissions are
handed out separately.

The symptom: a failure before any comparison even starts.

The fix: stop asking.

```bash
mito sync s3://acctA-bucket/data/@us-east-1 s3://acctB-bucket/data/@eu-west-1 \
  --source-profile acctA --dest-profile acctB
```

Pinning `@region` is worth doing anyway. It saves a round trip per bucket.

### 2. An S3-to-S3 copy needs a policy on the source bucket

This is the one that catches everybody. It is a property of S3, not of `mito`.

A server-side copy is **one API call, sent to the destination**, naming the source in the
`x-amz-copy-source` header. There is only one set of credentials on that request.

```
   ┌──────────────────────────────────────────────┐
   │  request: "copy acctA-bucket/x to here"      │
   │  sent to: acctB (the destination)            │
   │  signed with: --dest-profile                 │
   └──────────────────────────────────────────────┘
                       │
                       │  so the DESTINATION principal
                       │  must be able to read the SOURCE
                       ▼
   source bucket policy must grant acctB:
       s3:GetObject   on the objects
       s3:ListBucket  on the bucket
```

**`--source-profile` authenticates the source *listing* only.** It does not, and cannot,
authenticate the copy itself.

So a real cross-account copy also needs a source bucket policy:

```json
{
  "Version": "2012-10-17",
  "Statement": [{
    "Effect": "Allow",
    "Principal": { "AWS": "arn:aws:iam::DEST-ACCOUNT-ID:role/YourRole" },
    "Action": ["s3:GetObject", "s3:ListBucket"],
    "Resource": [
      "arn:aws:s3:::acctA-bucket",
      "arn:aws:s3:::acctA-bucket/*"
    ]
  }]
}
```

Without it you get a confusing partial failure: the listing succeeds, so `mito` knows exactly
which objects to move, and then cannot move a single one of them.

## What needs no extra grant

Only the server-side copy has this requirement. All of these are fine with per-side profiles
alone:

- **Cross-account `diff`.** Each side is read with its own profile, independently.
- **Upload.** Local to S3, one profile.
- **Download.** S3 to local, one profile.

The pattern is simple once you see it:

```
   bytes pass through your machine   ──►  each side authenticated separately,
                                          no cross-account request to authorize

   bytes move inside AWS             ──►  one request, one credential,
                                          needs the extra grant
```

A server-side copy is faster precisely *because* the bytes skip your machine. That is also
exactly why it needs the extra permission.

## Object ownership after a copy

An object copied into a bucket is owned by the account that wrote it, which is the **destination**
principal.

Depending on the destination bucket's Object Ownership setting, that can leave objects the bucket
owner cannot read. If you control the destination, setting **Bucket owner enforced** avoids the
whole class of problem.

`mito` sets no ACLs and does not try to manage ownership.
