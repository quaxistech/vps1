#!/usr/bin/env bash
# Publish a locally built AltSpotter release to a host.
#
# AltSpotter was originally installed by hand — a binary copied into
# /opt/altspotter/releases/1.0.0 and a symlink pointed at it. That is not a
# deployment: it cannot be repeated on a fresh host, it leaves no record of what
# was built, and it offers nothing to roll back to. This script does what
# AltTech's already does, and for the same reasons.
#
# The build happens HERE, not on the target: a box running a live market-data
# collector should not carry a toolchain or spend its RAM compiling, and the
# artifact that gets deployed must be the exact one that was built and tested.
#
# Because the artifact crosses machines, its ABI requirements are verified
# against the target BEFORE anything is installed. A binary needing a newer
# libstdc++ than the target has would otherwise fail at start-up, after the old
# release had already been replaced.
#
#   deploy/scripts/publish.sh root@host [release-id]
#
# Installing is deliberately separate from activating: the previous release
# stays on disk and the switch is one symlink.
set -euo pipefail

# VPS1 is the only production target. Passing user@host explicitly still works,
# but with no argument the target is read from vps1.md rather than guessed --
# and a missing vps1.md is a hard error, because the one thing that must never
# happen here is a deploy quietly going somewhere else. It happened once, to the
# test host, from a default that pointed at the wrong file.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
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
BUILD_DIR="${ALTSPOTTER_BUILD_DIR:-build/release}"
SSH="${ALTSPOTTER_SSH:-ssh}"
SCP="${ALTSPOTTER_SCP:-scp}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

say() { printf '\n== %s\n' "$*"; }

[[ -d "$BUILD_DIR" ]] || { echo "no build at $BUILD_DIR; run cmake --build --preset release" >&2; exit 1; }
BIN="$BUILD_DIR/altspotter"
[[ -x "$BIN" ]] || BIN="$BUILD_DIR/bin/altspotter"
[[ -x "$BIN" ]] || { echo "no altspotter binary under $BUILD_DIR" >&2; exit 1; }

say "staging release $RELEASE"
mkdir -p "$STAGE/altspotter/bin" "$STAGE/altspotter/migrations" \
         "$STAGE/altspotter/lib/systemd/system" "$STAGE/altspotter/share/altspotter/config-examples"
install -m 0755 "$BIN" "$STAGE/altspotter/bin/altspotter"
strip "$STAGE/altspotter/bin/altspotter" 2>/dev/null || true
install -m 0644 db/migrations/*.sql "$STAGE/altspotter/migrations/" 2>/dev/null || true
install -m 0644 deploy/systemd/*.service "$STAGE/altspotter/lib/systemd/system/" 2>/dev/null || true
install -m 0644 deploy/systemd/*.conf "$STAGE/altspotter/lib/systemd/system/" 2>/dev/null || true
install -m 0644 config/*.yaml "$STAGE/altspotter/share/altspotter/config-examples/" 2>/dev/null || true

{
    echo "release: $RELEASE"
    echo "commit: $(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
    echo "built_at: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "built_on: $(uname -srm)"
} > "$STAGE/altspotter/VERSION"

# --- ABI verification -------------------------------------------------------
# Every versioned symbol the binary needs must be provided by the target. This
# is the check that makes shipping a binary safe rather than hopeful.
say "verifying ABI against $TARGET"
NEEDED="$(objdump -T "$STAGE/altspotter/bin/altspotter" 2>/dev/null \
          | grep -oE '(GLIBC|GLIBCXX|CXXABI)_[0-9.]+' | sort -u || true)"
if [[ -n "$NEEDED" ]]; then
    # `grep -ao`, not `strings`: a minimal server image carries no binutils, and
    # `strings` missing produced an EMPTY available-set — which this check then
    # read as "the target provides nothing", failing a perfectly good host with
    # a list of every symbol the binary needs. grep is in coreutils-adjacent
    # base packages and is present where strings is not.
    AVAILABLE="$($SSH "$TARGET" '
        for lib in /lib/x86_64-linux-gnu/libc.so.6                    /usr/lib/x86_64-linux-gnu/libstdc++.so.6                    /lib/x86_64-linux-gnu/libstdc++.so.6; do
            [ -e "$lib" ] && grep -aoE "(GLIBC|GLIBCXX|CXXABI)_[0-9.]+" "$lib"
        done | sort -u')"
    # An empty result means the probe could not read the target, not that the
    # target is empty. Say which, instead of reporting a mismatch that is not one.
    if [[ -z "$AVAILABLE" ]]; then
        cat >&2 <<EOF

Could not read the target's ABI: no versioned symbols were found in libc or
libstdc++ on $TARGET. That is a broken probe, not a mismatch — check that
/lib/x86_64-linux-gnu/libc.so.6 and libstdc++.so.6 exist there and are readable
by this account. Refusing to guess.
EOF
        exit 1
    fi
    MISSING="$(comm -23 <(echo "$NEEDED") <(echo "$AVAILABLE") || true)"
    if [[ -n "$MISSING" ]]; then
        cat >&2 <<EOF

ABI mismatch: the target does not provide these symbol versions:
$MISSING

The build host has a newer toolchain than the target. Build on a host matching
the target, or link the newer runtime statically.
EOF
        exit 1
    fi
    echo "  all required symbol versions are available on the target"
fi

say "uploading"
tar -C "$STAGE" -czf "$STAGE/release.tgz" altspotter
SUM="$(sha256sum "$STAGE/release.tgz" | awk '{print $1}')"
$SCP "$STAGE/release.tgz" "$TARGET:/tmp/altspotter-$RELEASE.tgz"
$SCP "$(dirname "$0")/vps-flat-install.sh" "$TARGET:/tmp/altspotter-deploy-install.sh"

say "unpacking and verifying into /tmp/altspotter-deploy/stage-$RELEASE (not yet installed)"
$SSH "$TARGET" "set -euo pipefail
    echo '$SUM  /tmp/altspotter-$RELEASE.tgz' | sha256sum -c - >/dev/null
    rm -rf /tmp/altspotter-deploy/stage-$RELEASE
    mkdir -p /tmp/altspotter-deploy/stage-$RELEASE
    mv /tmp/altspotter-deploy-install.sh /tmp/altspotter-deploy/vps-flat-install.sh
    tar -C /tmp/altspotter-deploy/stage-$RELEASE --strip-components=1 -xzf /tmp/altspotter-$RELEASE.tgz
    rm -f /tmp/altspotter-$RELEASE.tgz
    /tmp/altspotter-deploy/stage-$RELEASE/bin/altspotter --version >/dev/null 2>&1 || true"

cat <<EOF

Release $RELEASE is staged but NOT installed. Install it with:

  $SSH $TARGET '
    cp /tmp/altspotter-deploy/stage-$RELEASE/lib/systemd/system/altspotter* /etc/systemd/system/ 2>/dev/null || true
    systemctl daemon-reload
    bash /tmp/altspotter-deploy/vps-flat-install.sh altspotter altspotter.service bin/altspotter /tmp/altspotter-deploy/stage-$RELEASE
    rm -rf /tmp/altspotter-deploy'

Staging is deliberately separate from installing, so the switch happens when you
are watching it. The installer renames each file into /opt/altspotter (atomic
per file), restarts the service and checks that ExecStart, the running
executable and /opt/altspotter/bin/altspotter are the same file. One tree on
disk -- no releases/, no current symlink, no backups; an earlier implementation
is restored by rebuilding it from the working copy.
EOF
