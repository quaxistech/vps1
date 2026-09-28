#!/bin/bash
# vps-flat-install.sh SERVICE UNIT MAIN_BIN [STAGED_DIR] [--no-restart]
#
# Runs ON the VPS, as root. One service = one installed tree = one binary set:
#
#   /opt/<service>/{bin,lib,share,...}      no releases/, no current symlink,
#                                          no backups, no .bak/.old/candidates
#
# 1. Migrates the old layout once: the tree `current` points at is moved up
#    into /opt/<service>, unit files are rewritten from /opt/<service>/current/
#    to /opt/<service>/, and `current` + `releases/` are removed after the
#    service is verified on the moved tree.
# 2. Installs STAGED_DIR (optional): every staged file is renamed into place
#    (atomic per file on one filesystem), owner and mode copied from the file it
#    replaces. STAGED_DIR is removed afterwards. Nothing old is kept: rollback
#    is a redeploy of an earlier build artifact from the build host.
#    When the staged tree carries SHA256SUMS it is the complete build: files
#    under /opt/<service> it does not list are removed (a file dropped by the
#    new build must not linger as an unowned old binary).
# 3. Restarts UNIT (unless --no-restart and the unit is not running, as on a
#    first install) and verifies: ExecStart path == running /proc/PID/exe ==
#    /opt/<service>/MAIN_BIN, and prints the running binary's sha256.
#
# Shipped in every service repository (identical copies); the deploy scripts
# upload it next to the release archive and run it on the unpacked tree.
set -euo pipefail
SVC=$1; UNIT=$2; MAIN=$3; STAGED=${4:-}; NO_RESTART=${5:-}
[ "$STAGED" = "--no-restart" ] && { STAGED=""; NO_RESTART="--no-restart"; }
ROOT=/opt/$SVC
[ -d "$ROOT" ] || { echo "no $ROOT"; exit 1; }

if [ -L "$ROOT/current" ]; then
    CUR=$(readlink -f "$ROOT/current")
    echo "migrate: $CUR -> $ROOT"
    for e in "$CUR"/* "$CUR"/.[!.]*; do
        [ -e "$e" ] || continue
        b=$(basename "$e")
        [ -e "$ROOT/$b" ] && { echo "refusing: $ROOT/$b already exists"; exit 1; }
        mv "$e" "$ROOT/$b"
    done
    for u in $(grep -rl "$ROOT/current/" /etc/systemd/system 2>/dev/null); do
        sed -i "s#$ROOT/current/#$ROOT/#g" "$u"
        echo "unit rewritten: $u"
    done
    systemctl daemon-reload
fi

if [ -n "$STAGED" ]; then
    COMPLETE=""
    [ -f "$STAGED/SHA256SUMS" ] && COMPLETE=$(cd "$STAGED" && find . -type f | sed 's#^\./##' | sort)
    (cd "$STAGED" && find . -type f) | while read -r f; do
        rel=${f#./}
        dst="$ROOT/$rel"
        mkdir -p "$(dirname "$dst")"
        tmp="$(dirname "$dst")/.$(basename "$dst").installing"
        cp "$STAGED/$rel" "$tmp"
        if [ -e "$dst" ]; then
            chown --reference="$dst" "$tmp"; chmod --reference="$dst" "$tmp"
        else
            ref=$(ls -d "$(dirname "$dst")"/* 2>/dev/null | grep -v '\.installing$' | head -1 || true)
            [ -n "$ref" ] && { chown --reference="$ref" "$tmp"; chmod --reference="$ref" "$tmp"; }
        fi
        mv -f "$tmp" "$dst"
        echo "installed: $rel"
    done
    rm -rf "$STAGED"
    if [ -n "$COMPLETE" ]; then
        (cd "$ROOT" && find . -type f ! -path './releases/*' | sed 's#^\./##' | sort) | while read -r f; do
            if ! grep -qxF "$f" <<<"$COMPLETE"; then rm -f "$ROOT/$f"; echo "removed stale: $f"; fi
        done
    fi
fi
if [ -f "$ROOT/SHA256SUMS" ]; then
    (cd "$ROOT" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS.new && mv -f SHA256SUMS.new SHA256SUMS)
fi

if [ "$NO_RESTART" = "--no-restart" ] && ! systemctl is-active --quiet "$UNIT"; then
    echo "installed, not started ($UNIT is not running)"
    rm -f "$ROOT/current"; rm -rf "$ROOT/releases"
    exit 0
fi
systemctl restart "$UNIT"
for i in $(seq 1 60); do systemctl is-active --quiet "$UNIT" && break; sleep 1; done
systemctl is-active "$UNIT"
PID=$(systemctl show -p MainPID --value "$UNIT")
EXE=$(readlink -f /proc/$PID/exe)
EXEC=$(systemctl show -p ExecStart "$UNIT" | grep -o 'path=[^ ;]*' | head -1 | cut -d= -f2)
echo "execstart=$(readlink -f "$EXEC")"
echo "exe=$EXE"
[ "$EXE" = "$ROOT/$MAIN" ] && [ "$(readlink -f "$EXEC")" = "$ROOT/$MAIN" ] || { echo "MISMATCH"; exit 1; }
echo "sha256=$(sha256sum "$EXE" | cut -c1-16)"

if [ -L "$ROOT/current" ] || [ -d "$ROOT/releases" ]; then
    rm -f "$ROOT/current"
    rm -rf "$ROOT/releases"
    echo "removed: current, releases/"
fi
find "$ROOT" \( -name '*.bak' -o -name '*.old' -o -name '*.new' -o -name '*.installing' -o -name '*candidate*' \) -print
echo "layout: $(ls "$ROOT" | tr '\n' ' ')"
