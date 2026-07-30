# Releasing

The release workflow publishes bytes people download and run, and a tag can only be pushed
once. So the process is built around rehearsing the whole thing first, then repeating it for
real with one input changed.

## 1. Preflight, locally

```sh
scripts/preflight-release.sh
```

Proves everything that does not need a runner: the tag shape, the tag agreeing with
`CMakeLists.txt` and `vcpkg.json`, the documented versions, the release docs matching the
workflow, the third-party notice inventory, and the rendered release notes. Add `--package`
to also package the host platform from an existing `build/` tree and verify the resulting
artifacts end to end.

It also asserts things about `release.yml` itself that no single job can check — most
importantly that a failed `linux-aarch64` build cannot withhold the release. That property is
load-bearing and easy to break by accident, because `needs: build` on a matrix job is
satisfied only when *every* leg succeeds.

## 2. Rehearse the real workflow

```sh
git tag v0.1.0-rc1
git push origin v0.1.0-rc1
```

A `-rcN` tag is **always a dry run**. It builds every platform, verifies every artifact and
its checksum after the upload/download round trip, and renders the release notes — then stops
without creating a GitHub Release or a provenance attestation.

That covers the parts a local preflight cannot: the cross-platform builds themselves, whether
an arm64 Linux runner is available on this plan at all, and the artifact hand-off between the
build matrix and the publish job.

Check the run, then clean up:

```sh
git push --delete origin v0.1.0-rc1
git tag -d v0.1.0-rc1
```

The same rehearsal against a tag that already exists:

```sh
gh workflow run release.yml -f tag=v0.1.0 -f dry_run=true
```

## 3. Release

```sh
git tag v0.1.0
git push origin v0.1.0
```

Identical to the rehearsal except that the publish step runs.

**Every `0.x` release is published as a prerelease.** The version is read from the tag, and
anything starting `0.` sets `prerelease: true` on the GitHub Release. That is deliberate: the
README, the book and `SECURITY.md` all say this is experimental and not for production, and a
release badged "Latest" would say the opposite to anyone landing on the repository. The preflight
asserts the publish step still sets it.

Reaching `1.0.0` will flip that automatically, which is the point at which you should mean it.

## Platform tiers

`linux-x86_64` and `macos-arm64` are **required**: if one is missing, the
publish job fails and names it, and nothing is created.

`linux-aarch64` is **best-effort**, because arm64 Linux runners are not offered on every
plan. If it is absent the release proceeds and logs a warning naming exactly what is missing;
if it is present it is verified as strictly as the required platforms. A release quietly
missing an architecture is worse than one that is loudly incomplete.

## What gets verified

`scripts/check-release-artifacts.sh` runs against every platform in the publish job and
inside `package-release.sh`. For each platform it checks the asset set and checksums, unpacks
the tarball and asserts its manifest, confirms the binary's architecture matches the platform
in the asset name — the publish runner cannot execute a foreign binary, so this reads the
ELF/Mach-O header — and verifies the `.deb` control and `.rpm` metadata declare the right
version and architecture.

## If a release goes wrong

Assets are attached to a GitHub Release, so deleting the release and its tag removes them.
Re-pushing a moved tag is handled: the `concurrency` group cancels the older run rather than
letting two runs upload assets to the same tag. Prefer bumping the patch version to re-cutting
a published tag — anyone who already downloaded the old bytes has no way to know they changed.
