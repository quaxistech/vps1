#!/usr/bin/env bash
# Publish a locally built AltML release to a host.
#
# The build happens here, not on the target. A four-core box that is already
# running a live feature pipeline, a label builder and an inference loop has no
# business also running a compiler, and the artifact that gets deployed should
# be the exact one that was built and tested rather than a second compilation of
# the same source.
#
# Because the artifact crosses machines, its ABI requirements are verified
# against the target *before* anything is installed: a binary needing a newer
# libstdc++ or glibc than the target provides would otherwise fail at start-up,
# with the previous release already replaced.
#
#   deploy/publish.sh root@host [release-id]
#
# Requires: a completed `cmake --build --preset release` and ssh access.
# Set ALTML_SSH / ALTML_SCP to override the transport.

set -euo pipefail

# Passing user@host explicitly sets the target. Without an argument, the target
# is read from vps1.md.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${1:-}" ]]; then
    TARGET="$1"
else
    VPS1="$REPO_ROOT/vps1.md"
    [[ -f "$VPS1" ]] || { echo "ERROR: no target given and $VPS1 is missing" >&2; exit 1; }
    _host="$(grep -oP '(?<=IP-адрес сервера: ).*' "$VPS1" | tr -d '[:space:]')"
    _user="$(grep -oP '(?<=Пользователь: ).*' "$VPS1" | tr -d '[:space:]')"
    [[ -n "$_host" && -n "$_user" ]] || { echo "ERROR: $VPS1 has no host/user" >&2; exit 1; }
    TARGET="$_user@$_host"
fi
RELEASE="${2:-$(date -u +%Y%m%dT%H%M%SZ)}"
BUILD_DIR="${ALTML_BUILD_DIR:-build/release}"
SSH="${ALTML_SSH:-ssh}"
SCP="${ALTML_SCP:-scp}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

say() { printf '\n== %s\n' "$*"; }

[[ -d "$BUILD_DIR" ]] || { echo "no build at $BUILD_DIR; run cmake --build --preset release" >&2; exit 1; }

say "staging release $RELEASE"
cmake --install "$BUILD_DIR" --prefix "$STAGE/altml" --strip >/dev/null

# The installed tree carries its own manifest, so a half-finished upload is
# detectable on the target instead of being discovered at start-up.
( cd "$STAGE/altml" && find . -type f ! -name SHA256SUMS -print0 \
    | sort -z | xargs -0 sha256sum > SHA256SUMS )

# --- ABI verification -------------------------------------------------------
say "verifying ABI against $TARGET"

required() {
    for binary in "$STAGE"/altml/bin/*; do
        objdump -T "$binary" 2>/dev/null |
            grep -oE '(GLIBC|GLIBCXX|CXXABI)_[0-9.]+[0-9]' || true
    done | sort -uV
}

# `strings` ships with binutils, which a minimal server image does not carry.
# Without it the pipeline yields NOTHING, every required symbol looks absent,
# and a perfectly compatible host is rejected -- including GLIBCXX_3.4, which
# every libstdc++ provides. `grep -a` reads the same strings from the .so and
# needs nothing installed.
available=$($SSH "$TARGET" '
    grep -haoE "(GLIBC|GLIBCXX|CXXABI)_[0-9]+(\.[0-9]+)*" \
        /usr/lib/x86_64-linux-gnu/libstdc++.so.6 \
        /lib/x86_64-linux-gnu/libc.so.6 \
        /lib/x86_64-linux-gnu/libgcc_s.so.1 2>/dev/null | sort -uV')

# An empty probe means "could not read the target", not "the target has none".
if [[ -z "$available" ]]; then
    echo "Could not read the target's ABI: no symbol versions found in its" >&2
    echo "libstdc++/libc/libgcc. That is a broken probe, not a broken target." >&2
    exit 1
fi

missing=""
while read -r symbol; do
    [[ -z "$symbol" ]] && continue
    grep -qxF "$symbol" <<<"$available" || missing+="  $symbol"$'\n'
done <<<"$(required)"

if [[ -n "$missing" ]]; then
    cat >&2 <<EOF
ABI mismatch: the target does not provide these symbol versions:
$missing
The build host has a newer toolchain than the target. Build on a host matching
the target, or link the C++ runtime statically.
EOF
    exit 1
fi
echo "  all required symbol versions are available on the target"

# --- ship -------------------------------------------------------------------
say "uploading"
tar -C "$STAGE" -czf "$STAGE/release.tgz" altml
$SCP "$STAGE/release.tgz" "$TARGET:/tmp/altml-$RELEASE.tgz" >/dev/null

$SCP "$(dirname "$0")/vps-flat-install.sh" "$TARGET:/tmp/altml-deploy-install.sh" >/dev/null

say "staging as /tmp/altml-deploy/stage-$RELEASE (verified, not yet installed)"
$SSH "$TARGET" "set -e
    rm -rf /tmp/altml-deploy/stage-$RELEASE
    mkdir -p /tmp/altml-deploy/stage-$RELEASE
    mv /tmp/altml-deploy-install.sh /tmp/altml-deploy/vps-flat-install.sh
    tar -C /tmp/altml-deploy/stage-$RELEASE --strip-components=1 -xzf /tmp/altml-$RELEASE.tgz
    rm -f /tmp/altml-$RELEASE.tgz
    cd /tmp/altml-deploy/stage-$RELEASE && sha256sum --quiet -c SHA256SUMS"

cat <<EOF

Release $RELEASE is staged and verified but NOT installed. Install it with:

  $SSH $TARGET '
    cp /tmp/altml-deploy/stage-$RELEASE/lib/systemd/system/altml* /etc/systemd/system/ 2>/dev/null || true
    systemctl daemon-reload
    bash /tmp/altml-deploy/vps-flat-install.sh altml altml.service bin/altml /tmp/altml-deploy/stage-$RELEASE
    rm -rf /tmp/altml-deploy'

Staging is deliberately separate from installing, so the switch happens when you
are watching it. The installer renames each file into /opt/altml (atomic per
file), removes what this build no longer ships, restarts the service and checks
that ExecStart, the running executable and /opt/altml/bin/altml are the same
file. There is exactly one tree on disk -- no releases/, no current symlink, no
backups: a retained binary with no git history behind it cannot be mapped back
to a source state, and an earlier implementation is restored by rebuilding it.
EOF
