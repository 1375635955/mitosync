#!/bin/sh
# MitoSync installer. Downloads the matching no-sudo installer from GitHub Releases.
#
#   curl -fsSL https://raw.githubusercontent.com/echemythia/mitosync/main/install.sh | sh
#
# Config via env:
#   MITO_VERSION          release tag to install (default: latest; `latest` or vX.Y.Z only)
#   MITO_INSTALL_PREFIX   install prefix passed through to the downloaded installer
set -eu

REPO="echemythia/mitosync"
VERSION="${MITO_VERSION:-latest}"

fail() { echo "mito install: $*" >&2; exit 1; }

# --- platform detection -----------------------------------------------------
case "$(uname -s)" in
  Linux)  os="linux" ;;
  Darwin) os="macos" ;;
  *) fail "no prebuilt installer for $(uname -s). Linux and macOS are published." ;;
esac

case "$(uname -m)" in
  x86_64 | amd64)
    arch="x86_64"
    ;;
  aarch64)
    arch="aarch64"
    ;;
  arm64)
    if [ "$os" = "macos" ]; then
      arch="arm64"
    else
      arch="aarch64"
    fi
    ;;
  *) fail "no prebuilt installer for $(uname -m). x86_64 and ARM64 are published." ;;
esac

platform="$os-$arch"

# macOS ships Apple Silicon only, so there is no macos-x86_64 asset to fetch. Say so plainly
# here: otherwise the download below 404s and the user is left guessing whether the release is
# broken or their machine is unsupported.
if [ "$platform" = "macos-x86_64" ]; then
  fail "no prebuilt binary for Intel macOS; macOS releases are Apple Silicon only.
Build from source instead: https://github.com/echemythia/mitosync#build-from-source"
fi

# --- version validation -----------------------------------------------------
# MITO_VERSION is interpolated into the download URL. Accept only the release shapes the
# workflow can publish, so a copied environment value cannot redirect the download path.
bad_version() {
  fail "invalid MITO_VERSION '$VERSION': expected 'latest' or vMAJOR.MINOR.PATCH (e.g. v0.7.0)"
}

case "$VERSION" in
  latest)
    base="https://github.com/$REPO/releases/latest/download"
    asset="mito-$platform.install.sh"
    ;;
  v*.*.*)
    _v="${VERSION#v}"
    _maj="${_v%%.*}"
    _rest="${_v#*.}"
    _min="${_rest%%.*}"
    _pat="${_rest#*.}"
    for _f in "$_maj" "$_min" "$_pat"; do
      case "$_f" in '' | *[!0-9]*) bad_version ;; esac
    done
    unset _v _rest _maj _min _pat _f
    base="https://github.com/$REPO/releases/download/$VERSION"
    asset="mito-${VERSION#v}-$platform.install.sh"
    ;;
  *)
    bad_version
    ;;
esac

# --- download and verify ----------------------------------------------------
command -v curl >/dev/null 2>&1 || fail "curl is required"

if command -v sha256sum >/dev/null 2>&1; then
  hash_file() { sha256sum "$1" | awk '{print $1}'; }
elif command -v shasum >/dev/null 2>&1; then
  hash_file() { shasum -a 256 "$1" | awk '{print $1}'; }
else
  fail "sha256sum or shasum is required"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

echo "mito install: downloading $asset ($VERSION)"
curl -fsSL "$base/$asset" -o "$tmp/$asset" ||
  fail "download failed from $base/$asset"

curl -fsSL "$base/$asset.sha256" -o "$tmp/$asset.sha256" ||
  fail "checksum unavailable at $base/$asset.sha256; refusing to install unverified"

want="$(awk '{print $1; exit}' "$tmp/$asset.sha256")"
got="$(hash_file "$tmp/$asset")"
[ -n "$want" ] || fail "checksum file is empty"
[ "$want" = "$got" ] || fail "checksum mismatch (expected $want, got $got)"
echo "mito install: checksum ok"

# The downloaded file is the self-extracting installer produced by scripts/package-release.sh.
# Pass arguments through, so `sh -s -- --prefix DIR` works from the one-liner.
sh "$tmp/$asset" "$@"
