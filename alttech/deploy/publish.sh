#!/usr/bin/env bash
# Publish a locally built AltTech release to a host.
#
# The build happens here, not on the target: a server running a live market-data
# collector should not be spending its CPU and RAM on compilation, should not
# carry a build toolchain, and — most importantly — the artifact that gets
# deployed should be the exact one that was built and tested, not a second
# compilation of the same source.
#
# Because the artifact crosses machines, its ABI requirements are verified
# against the target *before* anything is installed. A binary that needs a newer
# libstdc++ or glibc than the target provides would otherwise fail at start-up,
# after the old release had already been replaced.
#
#   deploy/publish.sh root@host [release-id]
#
# Requires: a completed `cmake --build --preset release`, ssh access, and
# sshpass or a key. Set ALTTECH_SSH to override the ssh command.

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
RELEASE="${2:-$(date -u +%Y%m%d-%H%M%S)}"
BUILD_DIR="${ALTTECH_BUILD_DIR:-build/release}"
SSH="${ALTTECH_SSH:-ssh}"
SCP="${ALTTECH_SCP:-scp}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

say() { printf '\n== %s\n' "$*"; }

[[ -d "$BUILD_DIR" ]] || { echo "no build at $BUILD_DIR; run cmake --build --preset release" >&2; exit 1; }

say "staging release $RELEASE"
cmake --install "$BUILD_DIR" --prefix "$STAGE/alttech" --strip >/dev/null

# --- ABI verification -------------------------------------------------------
# Every versioned symbol the binaries need must be provided by the target. This
# is the check that makes shipping a binary safe rather than hopeful.
say "verifying ABI against $TARGET"

required() {
    for binary in "$STAGE"/alttech/bin/*; do
        objdump -T "$binary" 2>/dev/null |
            grep -oE '(GLIBC|GLIBCXX|CXXABI)_[0-9.]+[0-9]' || true
    done | sort -uV
}

# `strings` is part of binutils, which a minimal server image does not carry.
# When it is missing the pipeline produces NOTHING, every required symbol looks
# absent, and a perfectly compatible host is rejected -- including GLIBCXX_3.4,
# which every libstdc++ in existence provides. `grep -a` reads the same version
# strings straight out of the .so and needs nothing installed.
available=$($SSH "$TARGET" '
    grep -haoE "(GLIBC|GLIBCXX|CXXABI)_[0-9]+(\.[0-9]+)*" \
        /usr/lib/x86_64-linux-gnu/libstdc++.so.6 \
        /lib/x86_64-linux-gnu/libc.so.6 \
        /lib/x86_64-linux-gnu/libgcc_s.so.1 2>/dev/null | sort -uV')

# An empty probe is "I could not read the target", not "the target has nothing".
# Reporting the second as the first is how a good host gets refused.
if [[ -z "$available" ]]; then
    cat >&2 <<'EOF'
Could not read the target's ABI: no symbol versions were found in its
libstdc++/libc/libgcc. That is a broken probe, not a broken target -- check that
those libraries exist at the expected paths and are readable.
EOF
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
The build host has a newer toolchain than the target. Either build on a host
matching the target, or rebuild with -DALTTECH_PORTABLE_RUNTIME=ON to carry the
C++ runtime inside the binary.
EOF
    exit 1
fi
echo "  all required symbol versions are available on the target"

# --- ship -------------------------------------------------------------------
say "uploading"
tar -C "$STAGE" -czf "$STAGE/release.tgz" alttech
$SCP "$STAGE/release.tgz" "$TARGET:/tmp/alttech-$RELEASE.tgz" >/dev/null

$SCP "$(dirname "$0")/vps-flat-install.sh" "$TARGET:/tmp/alttech-deploy-install.sh" >/dev/null

say "staging as /tmp/alttech-deploy/stage-$RELEASE (not yet installed)"
$SSH "$TARGET" "set -e
    rm -rf /tmp/alttech-deploy/stage-$RELEASE
    mkdir -p /tmp/alttech-deploy/stage-$RELEASE
    mv /tmp/alttech-deploy-install.sh /tmp/alttech-deploy/vps-flat-install.sh
    tar -C /tmp/alttech-deploy/stage-$RELEASE --strip-components=1 -xzf /tmp/alttech-$RELEASE.tgz
    rm -f /tmp/alttech-$RELEASE.tgz"

cat <<EOF

Release $RELEASE is staged but NOT installed. Install it with:

  $SSH $TARGET '
    cp /tmp/alttech-deploy/stage-$RELEASE/lib/systemd/system/alttech* /etc/systemd/system/ && systemctl daemon-reload
    /tmp/alttech-deploy/stage-$RELEASE/bin/alttech-schema --check --schema /tmp/alttech-deploy/stage-$RELEASE/share/alttech/schema.sql
    bash /tmp/alttech-deploy/vps-flat-install.sh alttech alttech.service bin/alttech-service /tmp/alttech-deploy/stage-$RELEASE
    rm -rf /tmp/alttech-deploy'

Staging is deliberately separate from installing, so the switch happens when you
are watching it. The installer renames each file into /opt/alttech (atomic per
file), restarts the service and checks that ExecStart, the running executable
and /opt/alttech/bin/alttech-service are the same file. There is exactly one
tree on disk -- no releases/, no current symlink, no backups: a retained binary
with no git history behind it cannot be mapped back to a source state, and an
earlier implementation is restored by rebuilding it.
EOF
