#!/usr/bin/env bash
# Build a release binary for THIS host and package it as a release artifact.
#
#   scripts/package-release.sh                    # build, test, package into dist/
#   scripts/package-release.sh --skip-tests       # skip ctest (not recommended)
#   scripts/package-release.sh --build-dir build --skip-build   # package an existing build
#
# There is no fully static mode. It was built, and it does not work: see
# docs/static-linking.md.
#
# Produces, in dist/:
#   mito-<version>-<platform>.tar.gz          portable archive
#   mito-<version>-<platform>.tar.gz.sha256   archive checksum, in sha256sum(1) format
#   mito-<version>-<platform>.install.sh      no-sudo installer for ~/.local
#   mito-<platform>.install.sh                stable latest-download installer alias
#   mito-<version>-<platform>.deb             Linux Debian-family installer, when on Linux
#   mito-<version>-<platform>.rpm             Linux RPM-family installer, when rpmbuild exists
#
# The tarball holds the binary, LICENSE, THIRD-PARTY-NOTICES.md, README.md and a
# BUILD-INFO.txt recording what the binary actually requires at run time. The release
# workflow calls this same script, so a locally cut artifact and a CI one are produced
# by identical logic.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build-release"
OUT_DIR="dist"
VERSION=""
SKIP_BUILD=0
SKIP_TESTS=0
JOBS=""

die() { echo "package-release: $*" >&2; exit 1; }
note() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

write_sha256() {
  local file="$1"
  local dir base
  dir="$(dirname "$file")"
  base="$(basename "$file")"
  if command -v sha256sum >/dev/null; then
    (cd "$dir" && sha256sum "$base" > "$base.sha256")
  else
    (cd "$dir" && shasum -a 256 "$base" > "$base.sha256")
  fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    --version)   VERSION="${2:-}"; shift 2 ;;
    --build-dir) BUILD_DIR="${2:-}"; shift 2 ;;
    --out-dir)   OUT_DIR="${2:-}"; shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --skip-tests) SKIP_TESTS=1; shift ;;
    -j|--jobs)   JOBS="${2:-}"; shift 2 ;;
    -h|--help)   sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
done

# --- platform ---------------------------------------------------------------
# Asset names use the names people say out loud (macos-arm64), not the toolchain's
# spelling of them (arm64-apple-darwin23).
case "$(uname -s)" in
  Linux)  OS="linux" ;;
  Darwin) OS="macos" ;;
  *) die "unsupported OS $(uname -s); Linux and macOS are the published platforms" ;;
esac
case "$(uname -m)" in
  x86_64|amd64)  ARCH="x86_64" ;;
  aarch64|arm64) [ "$OS" = macos ] && ARCH="arm64" || ARCH="aarch64" ;;
  *) die "unsupported architecture $(uname -m)" ;;
esac
PLATFORM="$OS-$ARCH"

# --- version ----------------------------------------------------------------
# CMakeLists.txt is the source of truth. vcpkg.json carries the same number and is
# checked against it: they drift silently otherwise, and the one that ends up in the
# artifact name would then disagree with the one in the manifest.
cmake_version="$(sed -n 's/^project(mito VERSION \([0-9][0-9.]*\)).*/\1/p' CMakeLists.txt | head -1)"
[ -n "$cmake_version" ] || die "could not read the project version from CMakeLists.txt"
vcpkg_version="$(sed -n 's/.*"version-string"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' vcpkg.json | head -1)"
[ "$cmake_version" = "$vcpkg_version" ] ||
  die "version mismatch: CMakeLists.txt says $cmake_version, vcpkg.json says $vcpkg_version"

if [ -n "$VERSION" ]; then
  [ "$VERSION" = "$cmake_version" ] ||
    die "--version $VERSION does not match the tree's version $cmake_version; bump CMakeLists.txt and vcpkg.json, or drop --version"
else
  VERSION="$cmake_version"
fi

NAME="mito-$VERSION-$PLATFORM"

# --- preflight --------------------------------------------------------------
command -v cmake >/dev/null || die "cmake is not on PATH"
[ -f vcpkg/scripts/buildsystems/vcpkg.cmake ] ||
  die "the vcpkg submodule is not bootstrapped; run ./bootstrap.sh first"

git_sha="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
git_dirty=""
if ! git diff --quiet HEAD -- 2>/dev/null; then
  git_dirty=" (working tree modified)"
fi

# Reproducibility, as far as it is cheap: timestamps in the archive come from the commit
# rather than from the clock, so building the same commit twice on the same host gives the
# same tarball. The binary itself is not reproducible - that would need a good deal more.
SOURCE_DATE_EPOCH="$(git log -1 --format=%ct 2>/dev/null || date +%s)"
export SOURCE_DATE_EPOCH

note "mito $VERSION for $PLATFORM (commit $git_sha$git_dirty)"

# --- build ------------------------------------------------------------------
if [ "$SKIP_BUILD" -eq 0 ]; then
  # Configured explicitly rather than through the release preset: this needs its own build
  # directory (so it never clobbers a working build/) and two flags the preset does not set.
  # Everything else matches the preset.
  # -g is never passed, so a Release build carries no DWARF of its own. What it does carry
  # is every symbol name and a great deal of code that nothing reaches: a statically linked
  # AWS SDK contributes far more of both than this project does. Putting each function and
  # object in its own section lets the linker drop the unreferenced ones.
  configure_args=(
    -S . -B "$BUILD_DIR" -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/vcpkg/scripts/buildsystems/vcpkg.cmake"
    -DCMAKE_CXX_FLAGS="-ffunction-sections -fdata-sections"
  )
  # --gc-sections / -dead_strip collect what the section flags above made collectable, and
  # -s tells the linker to leave the symbol table out rather than write it and have strip
  # remove it afterwards.
  if [ "$OS" = linux ]; then
    # The AWS SDK, OpenSSL and the rest already link statically through vcpkg. Pulling in
    # libstdc++ and libgcc too leaves only glibc-family system libraries dynamic, which is
    # what decides how old a distribution the artifact still runs on.
    configure_args+=(-DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc -Wl,--gc-sections -s")
  else
    # Anything older than this cannot load the binary at all, so it is a deliberate floor
    # rather than whatever the build host happened to default to. -dead_strip is ld64's
    # --gc-sections; there is no -s, so the strip below does that half on macOS.
    configure_args+=(
      -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-12.0}"
      -DCMAKE_EXE_LINKER_FLAGS="-Wl,-dead_strip"
    )
  fi

  note "Configuring"
  cmake "${configure_args[@]}"

  note "Building"
  if [ -n "$JOBS" ]; then
    cmake --build "$BUILD_DIR" -j "$JOBS"
  else
    cmake --build "$BUILD_DIR"
  fi
fi

BIN="$BUILD_DIR/mito"
[ -x "$BIN" ] || die "no binary at $BIN (did the build run?)"
[ -s THIRD-PARTY-NOTICES.md ] || die "THIRD-PARTY-NOTICES.md is missing or empty"

# --- test -------------------------------------------------------------------
# Shipping an untested binary is the failure this guards. Serial on purpose: SyncTaskTest
# and LocalToLocalSyncTest share fixed temporary directories and collide under -j.
if [ "$SKIP_TESTS" -eq 0 ]; then
  note "Testing (serial)"
  ctest --test-dir "$BUILD_DIR" --output-on-failure
else
  echo "package-release: WARNING - tests skipped, this artifact is unverified" >&2
fi

# --- stage ------------------------------------------------------------------
note "Staging $NAME"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/$NAME"
cp "$BIN" "$STAGE/$NAME/mito"
cp LICENSE README.md THIRD-PARTY-NOTICES.md "$STAGE/$NAME/"

# Strip, then re-sign. On Apple Silicon every binary carries at least an ad-hoc signature
# and `strip` invalidates it, which leaves a binary the kernel refuses to exec ("killed:
# 9"). Re-signing ad-hoc afterwards restores it. Linux has no such coupling.
size_before="$(wc -c < "$STAGE/$NAME/mito")"
if [ "$OS" = macos ]; then
  # -S drops debug symbols, -x drops local ones. What stays is the small set of external
  # symbols the loader needs.
  strip -S -x "$STAGE/$NAME/mito"
  if command -v codesign >/dev/null; then
    codesign --force --sign - "$STAGE/$NAME/mito"
    codesign --verify --strict "$STAGE/$NAME/mito" || die "the ad-hoc signature did not verify"
  else
    echo "package-release: WARNING - no codesign; an arm64 artifact may not run" >&2
  fi
else
  # --strip-all covers symbols and any debug sections. .comment only records which compiler
  # built it, which BUILD-INFO.txt states properly.
  strip --strip-all --remove-section=.comment "$STAGE/$NAME/mito"
fi
size_after="$(wc -c < "$STAGE/$NAME/mito")"

# Say it rather than assume it. A release binary carrying DWARF or a symbol table is both
# needlessly large and more informative to anyone poking at it than it needs to be, and the
# flags above are easy to lose in a future edit without anyone noticing.
if [ "$OS" = linux ] && command -v readelf >/dev/null; then
  if readelf -S "$STAGE/$NAME/mito" 2>/dev/null | grep -qE '\.debug_|\.symtab'; then
    readelf -S "$STAGE/$NAME/mito" | grep -E '\.debug_|\.symtab' || true
    die "debug information or a symbol table survived the strip"
  fi
fi

# What the binary needs at run time, recorded next to it. Without this the answer lives
# only on the machine that built it.
{
  echo "mito $VERSION"
  echo "platform:  $PLATFORM"
  echo "commit:    $git_sha$git_dirty"
  echo "built:     $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "build host: $(uname -srm)"
  echo "compiler:  $("${CXX:-c++}" --version 2>/dev/null | head -1 || echo unknown)"
  echo "cmake:     $(cmake --version | head -1)"
  echo "size:      $size_after bytes, stripped (from $size_before)"
  echo "symbols:   stripped; no debug information"
  echo
  echo "runtime dependencies:"
  if [ "$OS" = linux ]; then
    ldd "$STAGE/$NAME/mito" 2>/dev/null | sed 's/^[[:space:]]*/  /' || echo "  (static)"
    # The oldest glibc this binary will load against, which is the real portability
    # question for a Linux artifact and is invisible from `ldd` alone.
    glibc_min=""
    if command -v objdump >/dev/null; then
      glibc_min="$(objdump -T "$STAGE/$NAME/mito" 2>/dev/null |
        grep -o 'GLIBC_[0-9][0-9.]*' | sort -Vu | tail -1 || true)"
    elif command -v readelf >/dev/null; then
      glibc_min="$(readelf -V "$STAGE/$NAME/mito" 2>/dev/null |
        grep -o 'GLIBC_[0-9][0-9.]*' | sort -Vu | tail -1 || true)"
    fi
    [ -n "$glibc_min" ] && echo && echo "minimum glibc: ${glibc_min#GLIBC_}"
  else
    otool -L "$STAGE/$NAME/mito" 2>/dev/null | tail -n +2 | sed 's/^[[:space:]]*/  /' || true
    echo
    echo "minimum macOS: ${MACOSX_DEPLOYMENT_TARGET:-12.0}"
  fi
} > "$STAGE/$NAME/BUILD-INFO.txt"

cat "$STAGE/$NAME/BUILD-INFO.txt"

make_deb() {
  local deb_arch deb tmp root installed_size control_archive data_archive binary_file
  case "$ARCH" in
    x86_64) deb_arch="amd64" ;;
    aarch64) deb_arch="arm64" ;;
    *) die "no Debian architecture mapping for $ARCH" ;;
  esac

  command -v ar >/dev/null || {
    echo "package-release: WARNING - ar is missing, skipping .deb package" >&2
    return
  }

  deb="$OUT_DIR/$NAME.deb"
  tmp="$STAGE/deb-build"
  root="$tmp/root"
  mkdir -p "$tmp/archive" "$root/DEBIAN" "$root/usr/bin" "$root/usr/share/doc/mito"

  install -m 0755 "$STAGE/$NAME/mito" "$root/usr/bin/mito"
  install -m 0644 README.md "$root/usr/share/doc/mito/README.md"
  install -m 0644 THIRD-PARTY-NOTICES.md "$root/usr/share/doc/mito/THIRD-PARTY-NOTICES.md"
  install -m 0644 "$STAGE/$NAME/BUILD-INFO.txt" "$root/usr/share/doc/mito/BUILD-INFO.txt"
  install -m 0644 LICENSE "$root/usr/share/doc/mito/copyright"

  installed_size="$(du -sk "$root/usr" | awk '{print $1}')"
  cat > "$root/DEBIAN/control" <<EOF
Package: mito
Version: $VERSION
Section: utils
Priority: optional
Architecture: $deb_arch
Maintainer: MitoSync Authors <noreply@github.com>
Installed-Size: $installed_size
Depends: libc6 (>= 2.28), libstdc++6, libgcc-s1
Homepage: https://github.com/echemythia/mitosync
Description: CRC32 verification and sync tool for local files and S3
 MitoSync compares local files and S3 objects chunk by chunk using CRC32
 checksums. It can diff, sync, remove S3 objects, and inspect incomplete
 multipart uploads.
EOF

  binary_file="$tmp/archive/debian-binary"
  control_archive="$tmp/archive/control.tar.gz"
  data_archive="$tmp/archive/data.tar.gz"
  printf '2.0\n' > "$binary_file"

  if tar --version 2>/dev/null | grep -qi gnu; then
    tar --sort=name --owner=0 --group=0 --numeric-owner \
        --mtime="@$SOURCE_DATE_EPOCH" \
        -C "$root/DEBIAN" -czf "$control_archive" ./control
    tar --sort=name --owner=0 --group=0 --numeric-owner \
        --mtime="@$SOURCE_DATE_EPOCH" \
        -C "$root" -czf "$data_archive" ./usr
  else
    tar --uid 0 --gid 0 -C "$root/DEBIAN" -czf "$control_archive" ./control
    tar --uid 0 --gid 0 -C "$root" -czf "$data_archive" ./usr
  fi

  (cd "$tmp/archive" && ar rcs "$deb" debian-binary control.tar.gz data.tar.gz)
  write_sha256 "$deb"
}

make_rpm() {
  local rpm_arch rpm_top spec built rpm
  case "$ARCH" in
    x86_64) rpm_arch="x86_64" ;;
    aarch64) rpm_arch="aarch64" ;;
    *) die "no RPM architecture mapping for $ARCH" ;;
  esac

  command -v rpmbuild >/dev/null || {
    echo "package-release: WARNING - rpmbuild is missing, skipping .rpm package" >&2
    return
  }

  rpm="$OUT_DIR/$NAME.rpm"
  rpm_top="$STAGE/rpmbuild"
  spec="$rpm_top/SPECS/mito.spec"
  mkdir -p "$rpm_top/BUILD" "$rpm_top/BUILDROOT" "$rpm_top/RPMS" \
           "$rpm_top/SOURCES" "$rpm_top/SPECS" "$rpm_top/SRPMS"

  install -m 0755 "$STAGE/$NAME/mito" "$rpm_top/SOURCES/mito"
  install -m 0644 README.md "$rpm_top/SOURCES/README.md"
  install -m 0644 THIRD-PARTY-NOTICES.md "$rpm_top/SOURCES/THIRD-PARTY-NOTICES.md"
  install -m 0644 LICENSE "$rpm_top/SOURCES/LICENSE"
  install -m 0644 "$STAGE/$NAME/BUILD-INFO.txt" "$rpm_top/SOURCES/BUILD-INFO.txt"

  cat > "$spec" <<EOF
Name: mito
Version: $VERSION
Release: 1%{?dist}
Summary: CRC32 verification and sync tool for local files and S3
License: Apache-2.0
URL: https://github.com/echemythia/mitosync
Requires: glibc >= 2.28
Requires: libstdc++
Requires: libgcc

%description
MitoSync compares local files and S3 objects chunk by chunk using CRC32
checksums. It can diff, sync, remove S3 objects, and inspect incomplete
multipart uploads.

%prep

%build

%install
mkdir -p %{buildroot}%{_bindir}
mkdir -p %{buildroot}%{_docdir}/mito
install -m 0755 %{_sourcedir}/mito %{buildroot}%{_bindir}/mito
install -m 0644 %{_sourcedir}/README.md %{buildroot}%{_docdir}/mito/README.md
install -m 0644 %{_sourcedir}/THIRD-PARTY-NOTICES.md %{buildroot}%{_docdir}/mito/THIRD-PARTY-NOTICES.md
install -m 0644 %{_sourcedir}/BUILD-INFO.txt %{buildroot}%{_docdir}/mito/BUILD-INFO.txt
install -m 0644 %{_sourcedir}/LICENSE %{buildroot}%{_docdir}/mito/LICENSE

%files
%{_bindir}/mito
%license %{_docdir}/mito/LICENSE
%doc %{_docdir}/mito/README.md
%doc %{_docdir}/mito/THIRD-PARTY-NOTICES.md
%doc %{_docdir}/mito/BUILD-INFO.txt
EOF

  rpmbuild -bb "$spec" --target "$rpm_arch" \
    --define "_topdir $rpm_top" \
    --define "_build_id_links none"
  built="$(find "$rpm_top/RPMS" -name 'mito-*.rpm' | head -1)"
  [ -n "$built" ] || die "rpmbuild completed but no RPM was produced"
  cp "$built" "$rpm"
  write_sha256 "$rpm"
}

make_self_installer() {
  local alias_installer installer
  installer="$OUT_DIR/$NAME.install.sh"
  alias_installer="$OUT_DIR/mito-$PLATFORM.install.sh"

  cat > "$installer" <<EOF
#!/bin/sh
# Self-extracting installer for mito $VERSION ($PLATFORM).
# Installs to \$HOME/.local by default and never uses sudo.
set -eu

name='$NAME'
version='$VERSION'
platform='$PLATFORM'
prefix="\${MITO_INSTALL_PREFIX:-}"

usage() {
  cat <<USAGE
Usage: \$0 [--prefix DIR]

Installs mito \$version for \$platform without sudo.

Options:
  --prefix DIR   install under DIR instead of \$HOME/.local
  -h, --help     show this help

Environment:
  MITO_INSTALL_PREFIX   default install prefix when --prefix is not passed
USAGE
}

while [ "\$#" -gt 0 ]; do
  case "\$1" in
    --prefix)
      [ "\$#" -ge 2 ] || { echo "mito installer: --prefix needs a value" >&2; exit 1; }
      prefix="\$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "mito installer: unknown option: \$1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [ -z "\$prefix" ]; then
  [ -n "\${HOME:-}" ] || { echo "mito installer: HOME is not set; pass --prefix" >&2; exit 1; }
  prefix="\$HOME/.local"
fi

case "\$prefix" in
  ""|"/")
    echo "mito installer: refusing unsafe prefix '\$prefix'" >&2
    exit 1
    ;;
esac

command -v awk >/dev/null || { echo "mito installer: awk is required" >&2; exit 1; }
command -v gzip >/dev/null || { echo "mito installer: gzip is required" >&2; exit 1; }
command -v tar >/dev/null || { echo "mito installer: tar is required" >&2; exit 1; }

tmp="\${TMPDIR:-/tmp}/mito-install.\$\$"
rm -rf "\$tmp"
mkdir -p "\$tmp"
trap 'rm -rf "\$tmp"' EXIT HUP INT TERM

payload_line="\$(awk '/^__MITO_TARBALL_BELOW__$/ { print NR + 1; exit 0 }' "\$0")"
[ -n "\$payload_line" ] || { echo "mito installer: embedded payload marker missing" >&2; exit 1; }

tail -n "+\$payload_line" "\$0" | gzip -dc | tar -xf - -C "\$tmp"

mkdir -p "\$prefix/bin" "\$prefix/share/doc/mito"
cp "\$tmp/\$name/mito" "\$prefix/bin/mito"
chmod 0755 "\$prefix/bin/mito"

for file in README.md LICENSE THIRD-PARTY-NOTICES.md BUILD-INFO.txt; do
  if [ -f "\$tmp/\$name/\$file" ]; then
    cp "\$tmp/\$name/\$file" "\$prefix/share/doc/mito/\$file"
    chmod 0644 "\$prefix/share/doc/mito/\$file"
  fi
done

echo "Installed mito \$version to \$prefix/bin/mito"
case ":\${PATH:-}:" in
  *":\$prefix/bin:"*) ;;
  *) echo "Add \$prefix/bin to PATH to run mito from any shell." ;;
esac
exit 0

__MITO_TARBALL_BELOW__
EOF
  cat "$TARBALL" >> "$installer"
  chmod 0755 "$installer"
  write_sha256 "$installer"

  cp "$installer" "$alias_installer"
  chmod 0755 "$alias_installer"
  write_sha256 "$alias_installer"
}

# The binary has to actually run before it is worth packaging. `--help` exits 0 and touches
# argument parsing, logging setup and the AWS SDK's static initialisers, which is where a
# broken link shows up.
note "Smoke test"
"$STAGE/$NAME/mito" --help > /dev/null || die "the staged binary does not run"

# --help proves almost nothing: it returns before the AWS SDK is initialised. A musl build
# of this tree passed --help, passed every static-linkage check, and then aborted inside
# aws-c-cal the moment anything touched the SDK (docs/static-linking.md). So drive a real
# comparison instead. A local-to-local diff needs no network and no credentials, but it does
# go through Aws::InitAPI, the HTTP client factory and the CRC32 path - which is the part
# that can be broken by how the binary was linked.
smoke="$STAGE/smoke"
mkdir -p "$smoke/a" "$smoke/b"
printf 'mitosync release smoke test\n' > "$smoke/a/file.txt"
cp "$smoke/a/file.txt" "$smoke/b/file.txt"
"$STAGE/$NAME/mito" diff "$smoke/a/" "$smoke/b/" > /dev/null ||
  die "the staged binary could not complete a local comparison (exit $?)"

# And the other direction: a real difference has to be reported as one. A binary that
# returns 0 for everything would sail through the check above.
printf 'different\n' > "$smoke/b/file.txt"
if "$STAGE/$NAME/mito" diff "$smoke/a/" "$smoke/b/" > /dev/null; then
  die "the staged binary reported two different files as matching"
fi

# The binary reports a version compiled in from CMakeLists.txt, and the artifact is named
# from the same file. Asserting they agree catches a stale build directory packaged under a
# new version number, which is otherwise invisible until someone runs the wrong binary.
reported="$("$STAGE/$NAME/mito" --version | awk '{print $2}')"
[ "$reported" = "$VERSION" ] ||
  die "the binary reports version '$reported' but this artifact is '$VERSION' (stale build directory?)"

# --- package ----------------------------------------------------------------
note "Packaging"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
TARBALL="$OUT_DIR/$NAME.tar.gz"

# gzip -n keeps the timestamp out of the gzip header; the tar flags keep ownership and
# mtimes out of the members. GNU tar and the bsdtar on macOS spell the sorting flag
# differently, hence the branch.
if tar --version 2>/dev/null | grep -qi gnu; then
  tar --sort=name --owner=0 --group=0 --numeric-owner \
      --mtime="@$SOURCE_DATE_EPOCH" \
      -C "$STAGE" -cf - "$NAME" | gzip -9 -n > "$TARBALL"
else
  tar --uid 0 --gid 0 -C "$STAGE" -cf - "$NAME" | gzip -9 -n > "$TARBALL"
fi

write_sha256 "$TARBALL"
make_self_installer

if [ "$OS" = linux ]; then
  note "Packaging Linux installers"
  make_deb
  make_rpm
fi

# --- verify what was produced -----------------------------------------------
# Unpack the artifact somewhere else and run it from there. A tarball that verifies its own
# checksum but holds a binary that will not start is exactly the release nobody notices
# until a user reports it.
note "Verifying the artifact"
CHECK="$(mktemp -d)"
trap 'rm -rf "$STAGE" "$CHECK"' EXIT
if command -v sha256sum >/dev/null; then
  (cd "$OUT_DIR" && sha256sum -c "$NAME.tar.gz.sha256")
else
  (cd "$OUT_DIR" && shasum -a 256 -c "$NAME.tar.gz.sha256")
fi
scripts/check-release-artifacts.sh \
  --dist-dir "$OUT_DIR" \
  --version "$VERSION" \
  --platform "$PLATFORM" \
  --linux-packages auto
tar -xzf "$TARBALL" -C "$CHECK"
"$CHECK/$NAME/mito" --help > /dev/null || die "the packaged binary does not run"
packaged_reported="$("$CHECK/$NAME/mito" --version | awk '{print $2}')"
[ "$packaged_reported" = "$VERSION" ] ||
  die "the packaged binary reports version '$packaged_reported' but this artifact is '$VERSION'"
[ -s "$CHECK/$NAME/LICENSE" ] || die "LICENSE missing from the artifact"
[ -s "$CHECK/$NAME/THIRD-PARTY-NOTICES.md" ] || die "THIRD-PARTY-NOTICES.md missing from the artifact"
[ -s "$CHECK/$NAME/BUILD-INFO.txt" ] || die "BUILD-INFO.txt missing from the artifact"
grep -Fx "mito $VERSION" "$CHECK/$NAME/BUILD-INFO.txt" >/dev/null ||
  die "BUILD-INFO.txt does not name version $VERSION"
grep -Fx "commit:    $git_sha$git_dirty" "$CHECK/$NAME/BUILD-INFO.txt" >/dev/null ||
  die "BUILD-INFO.txt does not name commit $git_sha$git_dirty"

if [ -f "$OUT_DIR/$NAME.install.sh" ]; then
  "$OUT_DIR/$NAME.install.sh" --prefix "$CHECK/self-prefix" > "$CHECK/self-install.log" ||
    die "the self-extracting installer failed"
  "$CHECK/self-prefix/bin/mito" --help > /dev/null ||
    die "the self-extracting installer binary does not run"
  [ -s "$CHECK/self-prefix/share/doc/mito/BUILD-INFO.txt" ] ||
    die "the self-extracting installer did not install BUILD-INFO.txt"
  [ -s "$CHECK/self-prefix/share/doc/mito/THIRD-PARTY-NOTICES.md" ] ||
    die "the self-extracting installer did not install THIRD-PARTY-NOTICES.md"
fi

if [ -f "$OUT_DIR/mito-$PLATFORM.install.sh" ]; then
  "$OUT_DIR/mito-$PLATFORM.install.sh" --help > /dev/null ||
    die "the stable self-extracting installer alias does not run"
fi

if [ "$OS" = linux ] && [ -f "$OUT_DIR/$NAME.deb" ]; then
  mkdir -p "$CHECK/deb" "$CHECK/deb-root"
  (cd "$CHECK/deb" && ar x "$OUT_DIR/$NAME.deb")
  tar -xzf "$CHECK/deb/data.tar.gz" -C "$CHECK/deb-root"
  "$CHECK/deb-root/usr/bin/mito" --help > /dev/null ||
    die "the Debian package binary does not run"
  [ -s "$CHECK/deb-root/usr/share/doc/mito/THIRD-PARTY-NOTICES.md" ] ||
    die "the Debian package did not install THIRD-PARTY-NOTICES.md"
fi

if [ "$OS" = linux ] && [ -f "$OUT_DIR/$NAME.rpm" ] && command -v rpm >/dev/null; then
  rpm -qp --queryformat '%{NAME} %{VERSION} %{ARCH}\n' "$OUT_DIR/$NAME.rpm" |
    grep -Fx "mito $VERSION $(uname -m)" >/dev/null ||
    die "the RPM metadata does not name mito $VERSION for $(uname -m)"
  rpm -qpl "$OUT_DIR/$NAME.rpm" | grep -Fx "/usr/share/doc/mito/THIRD-PARTY-NOTICES.md" >/dev/null ||
    die "the RPM package did not install THIRD-PARTY-NOTICES.md"
fi

note "Done"
find "$OUT_DIR" -maxdepth 1 -type f -name "mito*-$PLATFORM.*" -print | sort | xargs ls -lh
find "$OUT_DIR" -maxdepth 1 -type f -name "mito*-$PLATFORM.*.sha256" -print | sort | xargs cat
