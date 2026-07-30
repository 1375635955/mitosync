# Security policy

MitoSync holds AWS credentials and can delete objects from S3 buckets. If you have found a way to
make it do something it should not, please tell me.

## Supported versions

MitoSync is experimental (0.x). Only the latest release gets fixes. There are no backports.

## Reporting a vulnerability

**Please do not open a public issue for a security problem.**

Use [private vulnerability reporting](https://github.com/echemythia/mitosync/security/advisories/new)
to open a draft advisory. That keeps the report private until there is a fix, and it is the only
channel for this project.

If you cannot use it, open a normal issue saying only that you have a security report and need a
private channel. No details. I will open an advisory and invite you.

Please include what you did, what happened, what you expected, the version (`mito --version`) and
your platform.

## What to expect

This is a personal project maintained in spare time, so I cannot promise a response time. I will
acknowledge your report, tell you roughly when I expect to fix anything real, and credit you in
the advisory unless you would rather I did not. If I think it is not a vulnerability, I will say
why rather than going quiet.

## In scope

- Credentials leaking into a report, a log line, or the metrics file
- `rm` deleting something the command line did not name
- `sync --delete` removing a file that exists at the source
- A comparison reporting a match it did not actually verify
- A path traversal that writes outside the destination you specified
- Certificate verification being skipped or weakened

## Not vulnerabilities

**CRC32 is not a cryptographic hash.** It catches accidental corruption and truncation. It is no
defence against someone deliberately building two files with the same checksum, which is trivial
for a 32-bit CRC. That is a deliberate trade: S3 computes CRC32 for free on its own hardware,
which is the whole reason a remote comparison can skip the download.

**Reports contain paths, bucket names and object keys.** A report written with `-o` is created
with an ordinary open, so your `umask` sets its mode, an existing file is truncated, and a symlink
is followed. Treat `-o` like a shell redirection.

**`--allow-unverified-ranges` weakens a check on purpose.** It exists for S3-compatible services
that omit `Content-Range` on ranged reads, and the run warns when you use it.

Bugs in the AWS SDK, OpenSSL or Boost belong to those projects. If a dependency advisory affects
MitoSync specifically, I do want to hear about it.

## Out of scope

Anything that needs your AWS credentials or local write access to begin with, denial of service
through deliberately extreme flag values, and scanner output with no demonstrated impact.

---

[Security and privacy](https://echemythia.github.io/mitosync/reference/security.html) in the
documentation covers how credentials are handled, what is written where, and the safety gates on
destructive operations.
