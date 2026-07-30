#!/usr/bin/env bash
# Check that THIRD-PARTY-NOTICES.md covers what the release binary actually redistributes.
#
# The release tarball links most dependencies in, so the notice inventory is a legal
# statement about the shipped artifact rather than a courtesy list. Two ways it goes wrong:
# a dependency gets added and nobody writes a notice for it, or a dependency gets bumped and
# the version recorded next to its licence goes stale. Both are invisible without a check,
# because the file stays perfectly well-formed either way.
#
# Runs in two modes. The required-package list always runs and needs no build. When vcpkg's
# installed-package metadata is present, every installed package is additionally reconciled
# against the file, which is what catches a dependency nobody thought about.
set -euo pipefail

cd "$(dirname "$0")/.."

die() { echo "check-third-party-notices: $*" >&2; exit 1; }
note() { printf 'check-third-party-notices: %s\n' "$*"; }

NOTICES="THIRD-PARTY-NOTICES.md"
VCPKG_INSTALLED=""

usage() {
  cat <<USAGE
Check that THIRD-PARTY-NOTICES.md covers what the release binary redistributes.

Usage:
  scripts/check-third-party-notices.sh [--vcpkg-installed DIR]

Options:
  --vcpkg-installed DIR   vcpkg_installed tree to reconcile against.
                          Default: build/vcpkg_installed or ./vcpkg_installed if present.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --vcpkg-installed)
      [ "$#" -ge 2 ] || die "$1 needs a value"
      VCPKG_INSTALLED="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
done

[ -f "$NOTICES" ] || die "missing $NOTICES"

# Dependencies that reach a user inside the release binary. Derived from the Linux release
# link metadata; this is the set whose licences we are obliged to reproduce.
REQUIRED=(
  aws-sdk-cpp aws-crt-cpp
  aws-c-auth aws-c-cal aws-c-common aws-c-compression aws-c-event-stream
  aws-c-http aws-c-io aws-c-mqtt aws-c-s3 aws-c-sdkutils aws-checksums
  openssl s2n curl
  fmt nlohmann-json spdlog
  zlib zlib-ng
)

# Not redistributed, so deliberately absent from the notices. gtest is test-only and the
# vcpkg-* ports are build helpers that produce no code in the binary.
is_exempt() {
  case "$1" in
    gtest|vcpkg-*) return 0 ;;
    *) return 1 ;;
  esac
}

failures=0
report() { printf '%s\n' "$*" >&2; failures=$((failures + 1)); }

# --- what the notices claim -------------------------------------------------
# Every inventory entry is a backticked package name followed by its version, whether it sits
# in a bullet list or in prose (curl is written inline).
#
# Held as tab-separated "name<TAB>version" lines rather than an associative array. macOS ships
# bash 3.2, which has no `declare -A`, and this script runs on the macOS release builders.
NOTICED_LIST="$(grep -oE '`[a-z0-9][a-z0-9._+-]*` [0-9][0-9A-Za-z.+#-]*' "$NOTICES" |
  sed -E 's/^`([^`]*)` (.*)$/\1	\2/' | sort -u)"

# Prints the version the notices record for a package, or nothing if it is absent.
noticed_version() {
  printf '%s\n' "$NOTICED_LIST" | awk -F'\t' -v want="$1" '$1 == want { print $2; exit }'
}
noticed_names() { printf '%s\n' "$NOTICED_LIST" | cut -f1 | grep -v '^$'; }

[ -n "$NOTICED_LIST" ] || die "found no '\`package\` version' inventory entries in $NOTICES"

# Boost is recorded collectively rather than per-package: vcpkg installs dozens of boost-*
# header ports and listing each would obscure rather than inform.
BOOST_VERSION="$(sed -n 's/.*all at Boost \([0-9][0-9A-Za-z.]*\)\..*/\1/p' "$NOTICES" | head -1)"
[ -n "$BOOST_VERSION" ] || report "$NOTICES: no collective Boost version statement found"

# --- 1. every required dependency is named ----------------------------------
for pkg in "${REQUIRED[@]}"; do
  [ -n "$(noticed_version "$pkg")" ] ||
    report "$NOTICES: does not name \`$pkg\`, which the release binary links in"
done

# --- 2. reconcile against vcpkg, when its metadata is available -------------
if [ -z "$VCPKG_INSTALLED" ]; then
  for candidate in build/vcpkg_installed vcpkg_installed; do
    [ -d "$candidate" ] || continue
    VCPKG_INSTALLED="$candidate"
    break
  done
fi

reconciled=0
boost_mismatched=0
boost_found=""
if [ -n "$VCPKG_INSTALLED" ] && [ -d "$VCPKG_INSTALLED/vcpkg/info" ]; then
  # vcpkg records each installed package as info/<name>_<version>_<triplet>.list. Package
  # names cannot contain an underscore, so the first field is unambiguous.
  while IFS= read -r listfile; do
    base="$(basename "$listfile" .list)"
    name="${base%%_*}"
    rest="${base#*_}"
    version="${rest%%_*}"
    [ -n "$name" ] && [ -n "$version" ] || continue
    is_exempt "$name" && continue
    reconciled=$((reconciled + 1))

    case "$name" in
      boost-*)
        # Collapsed into one report after the loop: fifty boost ports moving together would
        # otherwise bury every other finding under fifty copies of the same sentence.
        if [ -n "$BOOST_VERSION" ] && [ "$version" != "$BOOST_VERSION" ]; then
          boost_mismatched=$((boost_mismatched + 1))
          boost_found="$version"
        fi
        continue
        ;;
    esac

    recorded="$(noticed_version "$name")"
    if [ -z "$recorded" ]; then
      report "$NOTICES: does not name \`$name\`, which vcpkg installed into the build"
    elif [ "$recorded" != "$version" ]; then
      report "$NOTICES: records \`$name\` $recorded but $version is installed"
    fi
  done < <(find "$VCPKG_INSTALLED/vcpkg/info" -name '*.list' | sort)

  [ "$boost_mismatched" -eq 0 ] ||
    report "$NOTICES: says Boost $BOOST_VERSION but $boost_mismatched of the installed boost-* ports are at $boost_found"

  # The other direction is informational only. s2n is Linux-only, so a macOS tree legitimately
  # lacks packages the notices must still describe for the Linux artifact.
  while IFS= read -r pkg; do
    [ -n "$pkg" ] || continue
    find "$VCPKG_INSTALLED/vcpkg/info" -name "${pkg}_*.list" | grep -q . ||
      note "\`$pkg\` is in the notices but not installed in this tree (expected for other platforms' packages)"
  done < <(noticed_names)
else
  note "no vcpkg_installed tree found; checked the required-package list only"
  note "build the project, or pass --vcpkg-installed DIR, to reconcile every installed package"
fi

[ "$failures" -eq 0 ] || die "$failures notice coverage problem(s) in $NOTICES"

if [ "$reconciled" -gt 0 ]; then
  note "${#REQUIRED[@]} required packages named; $reconciled installed packages reconciled; Boost at $BOOST_VERSION"
else
  note "${#REQUIRED[@]} required packages named in $NOTICES"
fi
