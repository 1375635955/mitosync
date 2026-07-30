#!/usr/bin/env bash
# Write the release notes body for a version to stdout.
#
# Lives in a script rather than inline in release.yml so the preflight can render exactly what
# a release would publish. Notes assembled in one place and previewed from another is how a
# release ships a body nobody read.
set -euo pipefail

cd "$(dirname "$0")/.."

VERSION="${1:-}"
[ -n "$VERSION" ] || { echo "usage: scripts/release-notes.sh VERSION" >&2; exit 1; }

# A CHANGELOG section for this version wins when there is one. There is no changelog in the
# tree today, so the fallback below is what normally runs.
if [ -f CHANGELOG.md ] && grep -qE "^## \[?${VERSION//./\\.}\]?" CHANGELOG.md; then
  awk -v ver="$VERSION" '
    $0 ~ "^## \\[?" ver "\\]?" {grab=1; next}
    grab && /^## / {exit}
    grab && /^\[.*\]: / {next}
    grab {print}
  ' CHANGELOG.md | sed -e '/./,$!d'
  exit 0
fi

cat <<NOTES
Prebuilt \`mito\` $VERSION for Linux and macOS.

The Linux binaries are built against glibc 2.28, so they run on RHEL 8,
Debian 10, Ubuntu 18.10 and anything newer. The vcpkg dependency stack
and GCC runtime libraries are linked statically; glibc-family system
libraries remain dynamic.

Linux users can install the \`.deb\` or \`.rpm\` directly. Anyone can
use the no-sudo \`.install.sh\` asset to install under \`~/.local\`.
The tarballs remain available for manual installs.

Each tarball holds the binary, LICENSE, README.md,
THIRD-PARTY-NOTICES.md and a BUILD-INFO.txt recording what that binary
needs at run time (minimum glibc or macOS version, and its dynamic
dependencies).

Verify a download before running it:

\`\`\`sh
sha256sum -c mito-$VERSION-linux-x86_64.tar.gz.sha256
\`\`\`

MitoSync is experimental (0.x) and not intended for production use.
See the README for what \`sync\` does and does not guarantee.
NOTES
