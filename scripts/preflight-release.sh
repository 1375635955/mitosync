#!/usr/bin/env bash
# Rehearse a release before the tag exists.
#
# Splits the release into what can be proven locally for free and what only a real workflow
# run can prove. Everything in the first half runs here in seconds; the second half is printed
# as the dispatch command that exercises it, because a cross-platform matrix build and an
# artifact upload/download round trip cannot be faked on one machine.
#
# The workflow assertions matter as much as the checks: they are what caught `publish` being
# gated on every matrix leg succeeding, which would have withheld a whole release when the
# best-effort linux-aarch64 runner was simply unavailable.
set -euo pipefail

cd "$(dirname "$0")/.."

TAG=""
DO_PACKAGE=0

die() { echo "preflight-release: $*" >&2; exit 1; }

usage() {
  cat <<USAGE
Rehearse a release before the tag exists.

Usage:
  scripts/preflight-release.sh [--tag vX.Y.Z] [--package]

Options:
  --tag TAG    tag to validate; default: v<declared version>
  --package    also package the host platform and verify the artifacts.
               Needs an existing build/ tree; does not rebuild or run tests.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --tag) [ "$#" -ge 2 ] || die "--tag needs a value"; TAG="$2"; shift 2 ;;
    --package) DO_PACKAGE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
done

pass=0
fail=0
ok()   { printf '  \033[32mok\033[0m    %s\n' "$*"; pass=$((pass + 1)); }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$*"; fail=$((fail + 1)); }
info() { printf '        %s\n' "$*"; }
phase() { printf '\n\033[1m%s\033[0m\n' "$*"; }

run_check() {
  local label="$1"; shift
  local out
  if out="$("$@" 2>&1)"; then
    ok "$label"
    [ -z "$out" ] || info "$(tail -1 <<<"$out")"
  else
    bad "$label"
    sed 's/^/        /' <<<"$out" | tail -12
  fi
}

# --- 1. version and tag -----------------------------------------------------
phase "Version and tag"

VERSION="$(sed -n 's/^project(mito VERSION \([0-9][0-9.]*\)).*/\1/p' CMakeLists.txt | head -1)"
[ -n "$VERSION" ] || die "could not read the project version from CMakeLists.txt"
[ -n "$TAG" ] || TAG="v$VERSION"

# The same two shapes the workflow accepts, so a tag that would be refused there is refused
# here instead of after a matrix build.
if [[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  ok "$TAG is a release tag"
  DRY=false
elif [[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+-rc[0-9]+$ ]]; then
  ok "$TAG is a release candidate; the workflow will force a dry run"
  DRY=true
else
  bad "$TAG is neither vMAJOR.MINOR.PATCH nor vMAJOR.MINOR.PATCH-rcN; the workflow would refuse it"
  DRY=false
fi

tag_version="${TAG#v}"; tag_version="${tag_version%-rc*}"
if [ "$tag_version" = "$VERSION" ]; then
  ok "$TAG matches the declared version $VERSION"
else
  bad "$TAG implies version $tag_version but the tree declares $VERSION"
fi

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
  info "tag $TAG already exists locally"
else
  info "tag $TAG does not exist yet, which is the point of running this"
fi

# --- 2. checks that gate the release ----------------------------------------
phase "Release checks"
run_check "documented versions agree"        scripts/check-docs-version.sh
run_check "release docs match the workflow"  scripts/check-docs-drift.sh
run_check "third-party notices cover deps"   scripts/check-third-party-notices.sh

# --- 3. the workflow itself -------------------------------------------------
phase "Release workflow"
if python3 -c 'import yaml' 2>/dev/null; then
  wf_out="$(python3 scripts/lib/inspect-release-workflow.py .github/workflows/release.yml)" && wf_rc=0 || wf_rc=$?
  while IFS='|' read -r status message; do
    [ -n "${status:-}" ] || continue
    case "$status" in
      ok) ok "$message" ;;
      fail) bad "$message" ;;
      *) info "$message" ;;
    esac
  done <<<"$wf_out"
  [ "$wf_rc" -eq 0 ] || true
else
  info "python3 with PyYAML not available; skipped the workflow assertions"
fi

# --- 4. release notes -------------------------------------------------------
phase "Release notes"
if notes="$(scripts/release-notes.sh "$VERSION")"; then
  ok "rendered $(wc -l <<<"$notes" | tr -d ' ') lines for $VERSION"
  sed 's/^/        │ /' <<<"$notes"
else
  bad "scripts/release-notes.sh failed"
fi

# --- 5. optionally package the host platform -------------------------------
if [ "$DO_PACKAGE" -eq 1 ]; then
  phase "Host-platform artifacts"
  if [ ! -x build/mito ]; then
    bad "build/mito does not exist; configure and build before --package"
  else
    out="$(mktemp -d)"
    trap 'rm -rf "$out"' EXIT
    # package-release.sh runs check-release-artifacts.sh itself, so a pass here means the
    # manifest, the checksums, the architecture and the package metadata all verified.
    if log="$(scripts/package-release.sh --skip-build --build-dir build --skip-tests \
                --out-dir "$out" 2>&1)"; then
      ok "packaged and verified $(find "$out" -maxdepth 1 -type f ! -name '*.sha256' | wc -l | tr -d ' ') assets"
      grep 'manifest ok' <<<"$log" | sed 's/^/        /' || true
    else
      bad "packaging failed"
      sed 's/^/        /' <<<"$log" | tail -15
    fi
  fi
fi

# --- 6. what only a real run can prove --------------------------------------
phase "Only a real workflow run can prove these"
cat <<REMAINING
        - the linux-aarch64 and macos-arm64 builds themselves
        - whether an arm64 Linux runner is available on this plan at all
        - the artifact upload/download round trip between build and publish
        - build provenance attestation (public repositories only)

        Rehearse all of it without publishing by pushing a candidate tag:

            git tag ${TAG%-rc*}-rc1 && git push origin ${TAG%-rc*}-rc1

        A -rcN tag is always a dry run: it builds every platform, verifies every
        artifact and renders the notes, and creates no GitHub Release. Delete it
        afterwards with:

            git push --delete origin ${TAG%-rc*}-rc1 && git tag -d ${TAG%-rc*}-rc1

        The equivalent against an existing tag:

            gh workflow run release.yml -f tag=$TAG -f dry_run=true
REMAINING

phase "Summary"
if [ "$fail" -eq 0 ]; then
  printf '  %s check(s) passed. %s is ready to rehearse.\n' "$pass" "$TAG"
else
  printf '  %s passed, \033[31m%s failed\033[0m. Fix the above before tagging.\n' "$pass" "$fail"
fi
[ "$fail" -eq 0 ]
