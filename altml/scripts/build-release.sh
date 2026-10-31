#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

VERSION="$(sed -n 's/^project(altml VERSION \([^ ]*\).*/\1/p' CMakeLists.txt)"
COMMIT="$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
SOURCE_TREE_SHA256="$({
    LC_ALL=C find \
        CMakeLists.txt CMakePresets.json Makefile .gitignore \
        README.md config db deploy docs proto scripts src tests \
        -type f -print0 | \
        sort -z | xargs -0 sha256sum
} | sha256sum | awk '{print $1}')"
RELEASE="altml-${VERSION}-linux-x86_64"
STAGE="$REPO_ROOT/build/release-stage/$RELEASE"

cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure

rm -rf -- "$STAGE"
mkdir -p -- "$STAGE" "$REPO_ROOT/dist"
cmake --install "$REPO_ROOT/build/release" --prefix "$STAGE" --strip

install -m 0644 "$REPO_ROOT/build/release/altml_schema.desc" \
    "$STAGE/share/altml/proto/altml_schema.desc"

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
    zstd -9 -T0 -q -f -o "$REPO_ROOT/dist/$RELEASE.tar.zst"
sha256sum "$REPO_ROOT/dist/$RELEASE.tar.zst" > \
    "$REPO_ROOT/dist/$RELEASE.tar.zst.sha256"
echo "release ready: dist/$RELEASE.tar.zst"

# Remove temporary packaging files after successful archive creation.
rm -rf -- "$STAGE"
rmdir -- "$(dirname "$STAGE")" 2>/dev/null || true
