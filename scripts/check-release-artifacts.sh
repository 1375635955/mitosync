#!/usr/bin/env bash
# Verify that a release output directory contains the assets and in-archive files we publish.
set -euo pipefail

DIST_DIR="dist"
VERSION=""
PLATFORM=""
LINUX_PACKAGES="auto"

die() { echo "check-release-artifacts: $*" >&2; exit 1; }
note() { printf 'check-release-artifacts: %s\n' "$*"; }

usage() {
  cat <<USAGE
Verify that a release output directory contains the assets and in-archive files we publish.

Usage:
  scripts/check-release-artifacts.sh --version VERSION --platform PLATFORM [options]

Options:
  --dist-dir DIR             artifact directory, default: dist
  --linux-packages MODE      auto, required, or skip; default: auto

Examples:
  scripts/check-release-artifacts.sh --version 0.1.0 --platform linux-x86_64
  scripts/check-release-artifacts.sh --version 0.1.0 --platform macos-arm64 --linux-packages skip
USAGE
}

# Every option takes a value, so check for one before shifting past it. Letting `shift 2`
# run off the end would exit through set -e with no message at all.
need_value() {
  [ "$2" -ge 2 ] || die "$1 needs a value (try --help)"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dist-dir) need_value "$1" "$#"; DIST_DIR="$2"; shift 2 ;;
    --version) need_value "$1" "$#"; VERSION="$2"; shift 2 ;;
    --platform) need_value "$1" "$#"; PLATFORM="$2"; shift 2 ;;
    --linux-packages) need_value "$1" "$#"; LINUX_PACKAGES="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
done

[ -n "$VERSION" ] || die "--version is required"
[ -n "$PLATFORM" ] || die "--platform is required"
[ -d "$DIST_DIR" ] || die "artifact directory does not exist: $DIST_DIR"
DIST_DIR="$(cd "$DIST_DIR" && pwd)"
case "$LINUX_PACKAGES" in
  auto|required|skip) ;;
  *) die "--linux-packages must be auto, required, or skip" ;;
esac

NAME="mito-$VERSION-$PLATFORM"

# What the binary inside every artifact for this platform has to be. Anything not listed
# here leaves EXPECT_KIND empty, which skips the assertion rather than guessing wrong.
case "$PLATFORM" in
  linux-x86_64)  EXPECT_KIND="elf:x86_64";    DEB_ARCH="amd64"; RPM_ARCH="x86_64" ;;
  linux-aarch64) EXPECT_KIND="elf:aarch64";   DEB_ARCH="arm64"; RPM_ARCH="aarch64" ;;
  macos-x86_64)  EXPECT_KIND="macho:x86_64";  DEB_ARCH="";      RPM_ARCH="" ;;
  macos-arm64)   EXPECT_KIND="macho:arm64";   DEB_ARCH="";      RPM_ARCH="" ;;
  *) EXPECT_KIND=""; DEB_ARCH=""; RPM_ARCH=""
     note "unrecognised platform $PLATFORM; skipping architecture assertions" ;;
esac

# One temp root for the whole run, removed however we exit. The per-check temp dirs used to
# leak on the failure paths, because die() exits before any cleanup line is reached.
WORK=""
cleanup() { [ -z "$WORK" ] || rm -rf "$WORK"; }
trap cleanup EXIT HUP INT TERM
WORK="$(mktemp -d)"

need_file() {
  local rel="$1"
  [ -s "$DIST_DIR/$rel" ] || die "missing or empty $DIST_DIR/$rel"
}

check_sha() {
  local rel="$1"
  need_file "$rel"
  need_file "$rel.sha256"
  if command -v sha256sum >/dev/null; then
    (cd "$DIST_DIR" && sha256sum -c "$rel.sha256") >/dev/null
  elif command -v shasum >/dev/null; then
    (cd "$DIST_DIR" && shasum -a 256 -c "$rel.sha256") >/dev/null
  else
    die "sha256sum or shasum is required"
  fi
}

require_extracted_file() {
  local root="$1"
  local rel="$2"
  [ -s "$root/$rel" ] || die "$rel missing from extracted artifact"
}

# Read the machine type out of the executable header as "<format>:<arch>".
#
# The publish job runs on x86_64 Linux and cannot execute a macOS or aarch64 binary, so this
# is the only place a cross-build mixup can be caught: make_deb/make_rpm derive their
# architecture from the host $ARCH rather than from the binary, so an aarch64 release holding
# an x86_64 binary passes every other check and ships. readelf and otool are not both present
# everywhere, so read the bytes directly - od is in coreutils and in the macOS base system.
binary_kind() {
  local file="$1" hdr
  hdr="$(od -An -tx1 -N20 "$file" | tr -d ' \n')"
  case "$hdr" in
    # ELF: e_machine is a 2-byte little-endian field at offset 18.
    7f454c46*)
      case "${hdr:36:4}" in
        3e00) echo "elf:x86_64" ;;
        b700) echo "elf:aarch64" ;;
        *) echo "elf:unknown(${hdr:36:4})" ;;
      esac
      ;;
    # Mach-O 64-bit little-endian: cputype is a 4-byte little-endian field at offset 4.
    cffaedfe*)
      case "${hdr:8:8}" in
        07000001) echo "macho:x86_64" ;;
        0c000001) echo "macho:arm64" ;;
        *) echo "macho:unknown(${hdr:8:8})" ;;
      esac
      ;;
    cafebabe*) echo "macho-universal" ;;
    *) echo "unknown-format(${hdr:0:8})" ;;
  esac
}

check_binary_kind() {
  local file="$1" what="$2" found
  [ -n "$EXPECT_KIND" ] || return 0
  command -v od >/dev/null || die "od is required to check the architecture of $what"
  found="$(binary_kind "$file")"
  [ "$found" = "$EXPECT_KIND" ] ||
    die "$what is $found but $PLATFORM must be $EXPECT_KIND"
}

check_tarball() {
  local root="$WORK/tarball"
  mkdir -p "$root"
  tar -xzf "$DIST_DIR/$NAME.tar.gz" -C "$root"
  root="$root/$NAME"
  [ -d "$root" ] || die "$NAME root directory missing from tarball"
  require_extracted_file "$root" "mito"
  [ -x "$root/mito" ] || die "mito in tarball is not executable"
  check_binary_kind "$root/mito" "mito in $NAME.tar.gz"
  require_extracted_file "$root" "LICENSE"
  require_extracted_file "$root" "README.md"
  require_extracted_file "$root" "THIRD-PARTY-NOTICES.md"
  require_extracted_file "$root" "BUILD-INFO.txt"
  grep -Fx "mito $VERSION" "$root/BUILD-INFO.txt" >/dev/null ||
    die "BUILD-INFO.txt does not name mito $VERSION"
}

check_installer_help() {
  local rel="$1"
  sh "$DIST_DIR/$rel" --help >/dev/null || die "$rel --help failed"
}

check_deb() {
  local rel="$1"
  local base data control root
  command -v ar >/dev/null || die "ar is required to inspect $rel"
  base="$WORK/deb"
  mkdir -p "$base/archive" "$base/root" "$base/control"
  (cd "$base/archive" && ar x "$DIST_DIR/$rel")

  data="$base/archive/data.tar.gz"
  [ -s "$data" ] || die "$rel does not contain data.tar.gz"
  tar -xzf "$data" -C "$base/root"
  root="$base/root"
  require_extracted_file "$root" "usr/bin/mito"
  [ -x "$root/usr/bin/mito" ] || die "mito in $rel is not executable"
  check_binary_kind "$root/usr/bin/mito" "usr/bin/mito in $rel"
  require_extracted_file "$root" "usr/share/doc/mito/README.md"
  require_extracted_file "$root" "usr/share/doc/mito/THIRD-PARTY-NOTICES.md"
  require_extracted_file "$root" "usr/share/doc/mito/BUILD-INFO.txt"
  require_extracted_file "$root" "usr/share/doc/mito/copyright"

  # The control metadata is what apt believes about the package. A correct payload under a
  # wrong Architecture: still installs on the wrong machine.
  control="$base/archive/control.tar.gz"
  [ -s "$control" ] || die "$rel does not contain control.tar.gz"
  tar -xzf "$control" -C "$base/control"
  require_extracted_file "$base/control" "control"
  grep -Fx "Version: $VERSION" "$base/control/control" >/dev/null ||
    die "$rel control does not declare Version: $VERSION"
  if [ -n "$DEB_ARCH" ]; then
    grep -Fx "Architecture: $DEB_ARCH" "$base/control/control" >/dev/null ||
      die "$rel control does not declare Architecture: $DEB_ARCH"
  fi
}

check_rpm() {
  local rel="$1" required="$2"
  local listing meta
  if ! command -v rpm >/dev/null; then
    # Under --linux-packages=required the point is that this package was verified. Saying so
    # in a log line and exiting 0 is how an unverified rpm reaches a release.
    [ "$required" != required ] ||
      die "rpm is required to inspect $rel under --linux-packages=required"
    note "rpm is missing; verified $rel exists and its checksum only"
    return 0
  fi
  listing="$(rpm -qpl "$DIST_DIR/$rel" 2>/dev/null)" || die "rpm could not read $rel"
  local path
  for path in /usr/bin/mito \
              /usr/share/doc/mito/README.md \
              /usr/share/doc/mito/THIRD-PARTY-NOTICES.md \
              /usr/share/doc/mito/BUILD-INFO.txt; do
    grep -Fx "$path" <<<"$listing" >/dev/null || die "$rel does not install $path"
  done

  # Same reasoning as the deb control file: correct contents under wrong metadata still
  # installs on the wrong machine, or claims to be a version it is not.
  meta="$(rpm -qp --queryformat '%{VERSION} %{ARCH}' "$DIST_DIR/$rel" 2>/dev/null)" ||
    die "rpm could not read $rel metadata"
  [ "${meta% *}" = "$VERSION" ] ||
    die "$rel declares version '${meta% *}' but this artifact is '$VERSION'"
  if [ -n "$RPM_ARCH" ] && [ "${meta#* }" != "$RPM_ARCH" ]; then
    die "$rel declares arch '${meta#* }' but $PLATFORM must be '$RPM_ARCH'"
  fi
}

check_sha "$NAME.tar.gz"
check_tarball

check_sha "$NAME.install.sh"
check_installer_help "$NAME.install.sh"

check_sha "mito-$PLATFORM.install.sh"
check_installer_help "mito-$PLATFORM.install.sh"

if [[ "$PLATFORM" == linux-* && "$LINUX_PACKAGES" != skip ]]; then
  for ext in deb rpm; do
    rel="$NAME.$ext"
    if [ "$LINUX_PACKAGES" = required ] || [ -e "$DIST_DIR/$rel" ] || [ -e "$DIST_DIR/$rel.sha256" ]; then
      check_sha "$rel"
      case "$ext" in
        deb) check_deb "$rel" ;;
        rpm) check_rpm "$rel" "$LINUX_PACKAGES" ;;
      esac
    else
      note "$rel is absent; skipping because --linux-packages=auto"
    fi
  done
fi

note "$NAME manifest ok${EXPECT_KIND:+ ($EXPECT_KIND)}"
