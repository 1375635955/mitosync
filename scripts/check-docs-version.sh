#!/usr/bin/env bash
# Check that anything naming a specific mito version names the version this tree declares.
#
# scripts/package-release.sh already refuses a mismatch between CMakeLists.txt and
# vcpkg.json, but it only runs when someone packages a release. Docs go stale between those
# moments, and a release that ships install instructions pointing at the previous version is
# the kind of error every reader hits and no test catches.
set -euo pipefail

cd "$(dirname "$0")/.."

die() { echo "check-docs-version: $*" >&2; exit 1; }
note() { printf 'check-docs-version: %s\n' "$*"; }

SEMVER='[0-9]+\.[0-9]+\.[0-9]+'

# --- the version this tree declares -----------------------------------------
cmake_version="$(sed -n 's/^project(mito VERSION \([0-9][0-9.]*\)).*/\1/p' CMakeLists.txt | head -1)"
[ -n "$cmake_version" ] || die "could not read the project version from CMakeLists.txt"
vcpkg_version="$(sed -n 's/.*"version-string"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' vcpkg.json | head -1)"
[ -n "$vcpkg_version" ] || die "could not read version-string from vcpkg.json"
[ "$cmake_version" = "$vcpkg_version" ] ||
  die "version mismatch: CMakeLists.txt says $cmake_version, vcpkg.json says $vcpkg_version"
VERSION="$cmake_version"

# --- what to scan -----------------------------------------------------------
# THIRD-PARTY-NOTICES.md is deliberately absent: every version in it belongs to a dependency,
# not to mito, and pinning those to the project version would be wrong.
FILES=(README.md install.sh)
while IFS= read -r f; do FILES+=("$f"); done < <(find book/src -name '*.md' | sort)

# Text that names a mito version *specifically*. Each alternative is anchored on the word
# mito, on a release download URL, or on the v-prefixed tag spelling, so a dependency version
# such as "aws-c-cal 0.8.8" or "Boost 1.88.0" cannot match.
COMBINED="mito[ -]$SEMVER|releases/download/v$SEMVER|\\bv$SEMVER\\b"

# A line ending in "version-check:ignore" is exempt, for prose that deliberately cites an
# older version (a changelog entry, or a note about when something changed).
violations=0
checked=0
for f in "${FILES[@]}"; do
  [ -f "$f" ] || continue
  while IFS=: read -r lineno text; do
    [ -n "${lineno:-}" ] || continue
    case "$text" in *version-check:ignore*) continue ;; esac
    while IFS= read -r found; do
      [ -n "$found" ] || continue
      checked=$((checked + 1))
      [ "$found" = "$VERSION" ] && continue
      printf '%s:%s: names mito %s but this tree declares %s\n' \
        "$f" "$lineno" "$found" "$VERSION" >&2
      printf '    %s\n' "${text#"${text%%[![:space:]]*}"}" >&2
      violations=$((violations + 1))
    done < <(grep -oE "$COMBINED" <<<"$text" | grep -oE "$SEMVER" | sort -u)
  done < <(grep -nE "$COMBINED" "$f" || true)
done

if [ "$violations" -gt 0 ]; then
  die "$violations version mention(s) disagree with $VERSION; update them, make them version-agnostic (\`mito-<version>-...\`), or append 'version-check:ignore'"
fi

note "$checked version mention(s) across ${#FILES[@]} files agree with $VERSION"
