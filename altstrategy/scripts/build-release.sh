#!/usr/bin/env bash
# Build a self-contained native release archive for the single altstrategy
# service:
#   dist/altstrategy-<version>-linux-x86_64.tar.zst
# Contents: bin/ (altstrategy-service + operational tools), migrations/, proto/
# descriptor + sources, config-examples/, systemd/, README, VERSION, MANIFEST,
# SHA256SUMS. No secrets, no vps.md, no build cache, no runtime data.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

VERSION="$(grep -oP '(?<=project\(altstrategy VERSION )[0-9.]+' CMakeLists.txt)"
COMMIT="$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
BUILD_DIR="${BUILD_DIR:-build/release}"
RELEASE="altstrategy-${VERSION}-linux-x86_64"
STAGE="build/release-stage/${RELEASE}"

echo "==> configure + build"
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DALTSTRATEGY_BUILD_TESTS=OFF
cmake --build "$BUILD_DIR" --target \
    altstrategy-service altstrategy-db-migrate altstrategy-nats-init altstrategy-config-check altstrategy-version

echo "==> staging ${RELEASE}"
rm -rf "$STAGE"
mkdir -p "$STAGE"/{bin,migrations,proto,config-examples,systemd} dist

for b in altstrategy-service altstrategy-db-migrate altstrategy-nats-init altstrategy-config-check altstrategy-version; do
    cp "$BUILD_DIR/bin/$b" "$STAGE/bin/"
done
cp "$SCRIPT_DIR/lib-vps.sh" "$STAGE/bin/lib-vps.sh"
chmod 755 "$STAGE"/bin/*

# Strip VPS artifacts (debug symbols stay in the local build tree for diagnosis).
for artifact in "$STAGE"/bin/*; do
    if file -b "$artifact" | grep -q '^ELF '; then
        strip --strip-unneeded "$artifact"
    fi
done

cp db/migrations/*.sql "$STAGE/migrations/" 2>/dev/null || true
[[ -f "$BUILD_DIR/altstrategy_schema.desc" ]] && cp "$BUILD_DIR/altstrategy_schema.desc" "$STAGE/proto/"
cp proto/*.proto "$STAGE/proto/"
cp config/altstrategy.example.yaml config/common.example.yaml config/symbols.yaml \
    "$STAGE/config-examples/"
cp deploy/systemd/altstrategy.service "$STAGE/systemd/"
[[ -f README.md ]] && cp README.md "$STAGE/"

cat > "$STAGE/VERSION" <<EOF
service: altstrategy
version: ${VERSION}
commit: ${COMMIT}
built_at: $(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

echo "==> manifest + checksums"
( cd "$STAGE" && find . -type f ! -name SHA256SUMS -print0 | sort -z |
    xargs -0 sha256sum > SHA256SUMS )
( cd "$STAGE" && find . -type f | sort > MANIFEST )

echo "==> packing dist/${RELEASE}.tar.zst"
tar --use-compress-program='zstd -19' \
    -cf "dist/${RELEASE}.tar.zst" -C "build/release-stage" "$RELEASE"
ls -lh "dist/${RELEASE}.tar.zst"
echo "==> release built: dist/${RELEASE}.tar.zst (commit ${COMMIT})"
