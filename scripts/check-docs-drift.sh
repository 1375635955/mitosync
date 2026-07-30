#!/usr/bin/env bash
# Check that release-facing docs still describe the release we actually build.
#
# Two failure modes, both of which have already happened once:
#
#   1. Removed support comes back in prose. Fully static Linux artifacts were dropped
#      deliberately (docs/static-linking.md explains why), but README.md kept documenting a
#      `--static` flag and an Alpine build path long after they stopped existing.
#
#   2. The documented platform list and the published platform list drift apart. Docs listing
#      an asset nobody builds, or omitting one that ships, is invisible to every other check
#      because both files are individually valid.
set -euo pipefail

cd "$(dirname "$0")/.."

die() { echo "check-docs-drift: $*" >&2; exit 1; }
note() { printf 'check-docs-drift: %s\n' "$*"; }

RELEASE_WORKFLOW=".github/workflows/release.yml"
INSTALL_DOC="book/src/install.md"

# Release-facing docs only. docs/static-linking.md is deliberately excluded: its whole job is
# to discuss the static build that is not shipped, so every pattern below is expected there.
FILES=(README.md)
while IFS= read -r f; do FILES+=("$f"); done < <(find book/src -name '*.md' | sort)

failures=0
report() {
  printf '%s\n' "$*" >&2
  failures=$((failures + 1))
}

# --- 1. removed static-release support must not reappear ---------------------
# Deliberately narrow. "linked statically" is a true statement about the GCC runtime libs and
# appears in README.md today, so a blanket ban on the word "static" would be wrong.
check_pattern() {
  local label="$1" pattern="$2" f lineno text
  for f in "${FILES[@]}"; do
    [ -f "$f" ] || continue
    while IFS=: read -r lineno text; do
      [ -n "${lineno:-}" ] || continue
      case "$text" in *drift-check:ignore*) continue ;; esac
      report "$f:$lineno: $label"
      printf '    %s\n' "${text#"${text%%[![:space:]]*}"}" >&2
    done < <(grep -nEi "$pattern" "$f" || true)
  done
}

check_pattern "mentions a --static build flag, which no longer exists" '(^|[^-[:alnum:]])--static($|[^-[:alnum:]])'
check_pattern "documents a -static release asset, which is not published" 'mito-[a-z0-9._-]*-static'
check_pattern "documents an Alpine build path for releases" 'alpine'
check_pattern "mentions musl, which no release artifact uses" 'musl'
check_pattern "counts release artifacts in prose; the count changes with every platform" 'six (release )?artifacts'

# --- 2. documented platforms must match published platforms ------------------
[ -f "$RELEASE_WORKFLOW" ] || die "missing $RELEASE_WORKFLOW"
[ -f "$INSTALL_DOC" ] || die "missing $INSTALL_DOC"

# The workflow is the source of truth: the `for p in ...` loop is the required set, and the
# best-effort platform is whichever one the conditional branch names.
required="$(sed -n 's/.*for p in \(.*\); do.*/\1/p' "$RELEASE_WORKFLOW" | head -1)"
[ -n "$required" ] || die "could not read the required platform list from $RELEASE_WORKFLOW"
optional="$(grep -oE '\-\-platform +linux-aarch64' "$RELEASE_WORKFLOW" | head -1 | awk '{print $2}')"

published="$(printf '%s\n%s\n' "$(tr ' ' '\n' <<<"$required")" "$optional" | grep -v '^$' | sort -u)"

# What the install page tells people to download. The uname-based one-liner builds the name
# from $os/$arch and the downloaded-installer examples use a literal <version>, so neither
# matches: what is left is the explicit per-platform commands, which is exactly the list a
# reader picks from. Do not anchor this on linux|macos - a documented platform we never
# publish is the failure being looked for, so it has to be matchable whatever it is called.
documented="$(grep -oE 'mito-[a-z0-9_]+-[a-z0-9_]+\.install\.sh' "$INSTALL_DOC" |
  sed -E 's/^mito-//; s/\.install\.sh$//' | sort -u)"
[ -n "$documented" ] || die "found no per-platform install commands in $INSTALL_DOC"

while IFS= read -r p; do
  [ -n "$p" ] || continue
  grep -qxF "$p" <<<"$documented" ||
    report "$INSTALL_DOC: the release publishes $p but the install page does not mention it"
done <<<"$published"

while IFS= read -r p; do
  [ -n "$p" ] || continue
  grep -qxF "$p" <<<"$published" ||
    report "$INSTALL_DOC: documents $p, which $RELEASE_WORKFLOW does not publish"
done <<<"$documented"

[ "$failures" -eq 0 ] ||
  die "$failures drift problem(s); fix the docs, or append 'drift-check:ignore' if a mention is deliberate"

note "release docs match $(wc -l <<<"$published" | tr -d ' ') published platforms and reintroduce no removed static support"
