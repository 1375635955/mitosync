# Why there is no fully static Linux binary

*Investigated 2026-07-28, against aws-c-cal 0.8.8 as vendored by vcpkg.*

A fully static `mito` would be convenient: one file that runs on any Linux, in a container,
on a distribution older than the build host. It was built. It does not work, and the reason
is not fixable with a build flag.

## What happens

Built inside Alpine against musl with `-static`, the whole dependency tree — OpenSSL, Boost,
the AWS SDK and its `aws-c-*` libraries — compiles and links without complaint. The binary
is genuinely static, passes `--help`, and reports the right version.

Then anything that touches the AWS SDK aborts:

```
Fatal error condition occurred in .../aws-c-cal/source/unix/openssl_platform_init.c:647:
process && "Unable to load symbols from process space"
Exiting Application
```

85 of the 1755 tests fail this way. Every one of them is a test that initialises the SDK.

## Why

`aws-c-cal` discovers which libcrypto it is linked against by inspecting its own process
image at run time:

```c
void *process = dlopen(NULL, RTLD_NOW);
AWS_FATAL_ASSERT(process && "Unable to load symbols from process space");
result = s_resolve_libcrypto_symbols(AWS_LIBCRYPTO_LC, process);
```

In a fully static musl binary there is no dynamic loader. `dlopen` is a stub that returns
`NULL`, the fatal assert fires, and the process exits before any of our code runs. The
libcrypto symbols are present in the binary — they simply cannot be found the way this code
insists on finding them.

Neither `USE_OPENSSL=ON` nor `BYO_CRYPTO` avoids it: the resolution happens unconditionally
at init, not behind a compile-time branch.

## What it would take

An overlay port for `aws-c-cal` that patches out the `dlopen`-based resolution and binds the
statically linked libcrypto directly. That is a local patch to the code path that resolves
the crypto used to sign every S3 request, re-verified on every `aws-c-cal` bump. For a
convenience artifact, on a project whose users all have glibc, that trade was judged not
worth making.

Worth revisiting if `mito` ever needs to ship inside a `scratch`/distroless image or run on
Alpine hosts. If it does, the fix belongs upstream rather than in an overlay.

## What was done instead

The Linux artifacts are built in `quay.io/pypa/manylinux_2_28_x86_64`, which pairs glibc
2.28 with a current GCC. The vcpkg dependency stack and GCC runtime libraries are linked
statically, while glibc-family system libraries remain dynamic. That gives the artifact the
RHEL 8 / Debian 10 / Ubuntu 18.10 floor: most of the portability, none of the patched
dependencies. See `.github/workflows/release.yml`.

## The lesson that outlived the experiment

The static binary passed every check `scripts/package-release.sh` had at the time: it linked,
it was verifiably static, `--help` worked, the tarball extracted and ran. It would have been
published, and it would have died on the user's first real command.

`--help` returns before `Aws::InitAPI` is ever called. The script's smoke test now runs a
local-to-local `diff` instead, which needs no network and no credentials but does go through
SDK initialisation — and it checks both that identical files compare equal and that different
ones do not. That check fails on the musl binary, which is what a release gate is for.
