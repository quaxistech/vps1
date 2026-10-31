#!/usr/bin/env bash
# Build the immutable release archive locally. Only the finished binary, its
# config examples, migrations and unit ever travel to the VPS: that host has no
# -dev packages and no disk to spare for a toolchain.
#
# The archive is produced only after every test has passed.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

VERSION="$(sed -n 's/^project(altmacro VERSION \([^ ]*\).*/\1/p' CMakeLists.txt)"
COMMIT="$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
SOURCE_TREE_SHA256="$({
    LC_ALL=C find \
        CMakeLists.txt CMakePresets.json .gitignore \
        README.md config db deploy docs scripts src tests \
        -type f -print0 | sort -z | xargs -0 sha256sum
} | sha256sum | awk '{print $1}')"
RELEASE="altmacro-${VERSION}-linux-x86_64"
STAGE="$REPO_ROOT/build/release-stage/$RELEASE"

cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure

rm -rf -- "$STAGE"
mkdir -p -- "$STAGE" "$REPO_ROOT/dist"
# --strip omits debug symbols from the release archive; they remain available
# in the local build/ tree.
cmake --install "$REPO_ROOT/build/release" --prefix "$STAGE" --strip

{
    echo "version: $VERSION"
    echo "commit: $COMMIT"
    echo "source_tree_sha256: $SOURCE_TREE_SHA256"
    echo "built_at: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "target: ubuntu-24.04 x86_64"
} > "$STAGE/VERSION"

(cd "$STAGE" && find . -type f ! -name SHA256SUMS -print0 | sort -z | \
    xargs -0 sha256sum > SHA256SUMS)
tar -C "$REPO_ROOT/build/release-stage" -cf - "$RELEASE" | \
    zstd -19 -T0 -q -f -o "$REPO_ROOT/dist/$RELEASE.tar.zst"
sha256sum "$REPO_ROOT/dist/$RELEASE.tar.zst" > "$REPO_ROOT/dist/$RELEASE.tar.zst.sha256"
echo "release ready: dist/$RELEASE.tar.zst ($(du -h "$REPO_ROOT/dist/$RELEASE.tar.zst" | cut -f1))"

# Remove temporary packaging files after successful archive creation.
rm -rf -- "$STAGE"
rmdir -- "$(dirname "$STAGE")" 2>/dev/null || true
