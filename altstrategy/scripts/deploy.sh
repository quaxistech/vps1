#!/usr/bin/env bash
# Deploy the altstrategy release to the VPS as an independent service under
# /opt/altstrategy -- ONE installed tree, no releases/, no `current` symlink,
# no backups (rollback = redeploy an earlier build artifact from this host):
#   build -> checksum -> upload -> unpack into a staging dir -> verify SHA256SUMS
#   -> install non-secret config into /etc/altstrategy -> install the systemd
#   unit -> vps-flat-install.sh (per-file atomic rename into /opt/altstrategy,
#   stale files removed, restart if running, ExecStart == exe verified).
#
# It DOES NOT start or enable the service, DOES NOT run DB migrations, and DOES
# NOT touch NATS: the altstrategy schema is applied separately (altstrategy-db-
# migrate) and the AltStrategy-owned ALTSTRATEGY_DECISIONS JetStream stream is
# provisioned separately (altstrategy-nats-init). Connection details come from
# vps.md and are never printed. Production secrets in /etc are never overwritten.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib-vps.sh"

exec 9>/tmp/altstrategy-deploy.lock
flock -n 9 || { echo "ERROR: another local altstrategy deploy is running" >&2; exit 1; }

SKIP_BUILD="${SKIP_BUILD:-0}"
VERSION="$(grep -oP '(?<=project\(altstrategy VERSION )[0-9.]+' "$REPO_ROOT/CMakeLists.txt")"
RELEASE_BASE="altstrategy-${VERSION}-linux-x86_64"
ARCHIVE="$REPO_ROOT/dist/${RELEASE_BASE}.tar.zst"

if [[ "$SKIP_BUILD" != "1" ]]; then
    "$SCRIPT_DIR/build-release.sh"
fi
[[ -f "$ARCHIVE" ]] || { echo "ERROR: release archive missing: $ARCHIVE" >&2; exit 1; }

SUM="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
STAMP="$(date -u +%Y%m%d%H%M%S)"
RELEASE_DIR="/tmp/altstrategy-deploy/stage-${STAMP}"

# A binary needing a newer libstdc++ or glibc than the target provides fails at
# start-up, after the previous release has already been replaced. Ported here
# when deploy/publish.sh was removed as a duplicate entry point: that script
# carried this check and this one did not, and losing it to tidy up the script
# list would have traded a real safeguard for a shorter directory listing.
#
# Symbol-version families are cumulative and totally ordered, so the question is
# "is the target's newest at least as new as what we need", not "is this exact
# string present" -- comparing exact strings reports a false mismatch, because a
# target defining CXXABI_1.3.9 satisfies a binary requiring CXXABI_1.3.
echo "==> verifying ABI against the target"
abi_required() {
    for binary in "$REPO_ROOT"/build/release/bin/*; do
        [[ -x "$binary" ]] || continue
        objdump -T "$binary" 2>/dev/null |
            grep -oE '(GLIBC|GLIBCXX|CXXABI)_[0-9.]+[0-9]' || true
    done | sort -uV
}
abi_available="$(vps_ssh '
    for lib in /usr/lib/x86_64-linux-gnu/libstdc++.so.6 \
               /lib/x86_64-linux-gnu/libc.so.6 \
               /lib/x86_64-linux-gnu/libgcc_s.so.1; do
        [ -r "$lib" ] || continue
        grep -haoE "(GLIBC|GLIBCXX|CXXABI)_[0-9.]+[0-9]" "$lib"
    done | sort -uV')"
if [[ -z "$abi_available" ]]; then
    echo "ERROR: could not read the target ABI; refusing to install" >&2
    exit 1
fi
abi_newest() { grep -E "^$1_" | sort -V | tail -1; }
abi_missing=""
abi_need_all="$(abi_required)"
for family in GLIBC GLIBCXX CXXABI; do
    need="$(abi_newest "$family" <<<"$abi_need_all")"
    have="$(abi_newest "$family" <<<"$abi_available")"
    [[ -z "$need" ]] && continue
    if [[ -z "$have" ]]; then
        abi_missing+="  $need (target defines no $family versions at all)"$'\n'
        continue
    fi
    oldest="$(printf '%s\n%s\n' "$need" "$have" | sort -V | head -1)"
    if [[ "$oldest" != "$need" && "$need" != "$have" ]]; then
        abi_missing+="  $need (target has $have)"$'\n'
    fi
done
if [[ -n "$abi_missing" ]]; then
    echo "ERROR: ABI mismatch, the target does not provide:" >&2
    printf '%s' "$abi_missing" >&2
    echo "Build on a host matching the target, or link the C++ runtime statically." >&2
    exit 1
fi
echo "    all required symbol versions are available on the target"

echo "==> uploading release ($(du -h "$ARCHIVE" | cut -f1))"
vps_ssh "mkdir -p /opt/altstrategy /tmp/altstrategy-deploy"
vps_scp "$ARCHIVE" "/tmp/altstrategy-deploy/${RELEASE_BASE}.tar.zst"
vps_scp "$SCRIPT_DIR/vps-flat-install.sh" "/tmp/altstrategy-deploy/vps-flat-install.sh"

echo "==> unpacking and verifying on VPS"
vps_ssh "set -euo pipefail
    echo '${SUM}  /tmp/altstrategy-deploy/${RELEASE_BASE}.tar.zst' | sha256sum -c - >/dev/null
    stage='${RELEASE_DIR}.staging'
    [[ ! -e '${RELEASE_DIR}' && ! -e \"\$stage\" ]]
    cleanup_stage() { rm -rf -- \"\$stage\"; }
    trap cleanup_stage EXIT
    mkdir -p \"\$stage\"
    tar --use-compress-program=unzstd -xf /tmp/altstrategy-deploy/${RELEASE_BASE}.tar.zst \
        -C \"\$stage\" --strip-components=1
    cd \"\$stage\" && sha256sum --quiet -c SHA256SUMS
    chown -R root:root \"\$stage\"
    mv \"\$stage\" '${RELEASE_DIR}'
    trap - EXIT
    rm -f /tmp/altstrategy-deploy/${RELEASE_BASE}.tar.zst"

echo "==> installing non-secret configuration into /etc/altstrategy (never overwrites)"
vps_ssh "set -euo pipefail
    getent group altstrategy >/dev/null || { echo 'ERROR: group altstrategy missing' >&2; exit 1; }
    install -d -o root -g altstrategy -m 0750 /etc/altstrategy
    for src in '${RELEASE_DIR}'/config-examples/altstrategy.example.yaml \
               '${RELEASE_DIR}'/config-examples/common.example.yaml; do
        name=\$(basename \"\$src\" .example.yaml)
        dst=\"/etc/altstrategy/\$name.yaml\"
        [[ -e \"\$dst\" ]] || install -o root -g altstrategy -m 0640 \"\$src\" \"\$dst\"
    done
    sdst=/etc/altstrategy/symbols.yaml
    [[ -e \"\$sdst\" ]] || install -o root -g altstrategy -m 0640 \
        '${RELEASE_DIR}/config-examples/symbols.yaml' \"\$sdst\""

echo "==> config check (shared common config, new release binary)"
vps_ssh "set -euo pipefail
    set -a; . /etc/altstrategy/common.env; set +a
    '${RELEASE_DIR}/bin/altstrategy-config-check' --file /etc/altstrategy/common.yaml --kind common || \
        echo 'WARN: config-check reported issues (review before starting)'"

echo "==> provisioning self-contained state directories"
vps_ssh "set -euo pipefail
    for d in archive exports exports/signal-quality cache; do
        install -d -o altstrategy -g altstrategy -m 0750 \"/var/lib/altstrategy/\$d\"
    done"

echo "==> installing systemd unit (disabled, not started)"
vps_ssh "set -euo pipefail
    install -o root -g root -m 0644 '${RELEASE_DIR}/systemd/altstrategy.service' \
        /etc/systemd/system/altstrategy.service
    systemctl daemon-reload"

echo "==> installing into /opt/altstrategy (one tree; restarts only a running service)"
vps_ssh "set -euo pipefail
    bash /tmp/altstrategy-deploy/vps-flat-install.sh altstrategy altstrategy.service \
        bin/altstrategy-service '${RELEASE_DIR}' --no-restart"

vps_ssh "rm -rf /tmp/altstrategy-deploy"
echo "==> deploy OK: /opt/altstrategy (${VERSION}, archive sha256 ${SUM:0:16})"
echo "    a running service was restarted and verified; a stopped one was left stopped:"
echo "    start with:  systemctl enable --now altstrategy.service"
