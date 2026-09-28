#!/usr/bin/env bash
# Deploy the AltSignalQuality release to the VPS as an independent service under
# /opt/altsignalquality -- ONE installed tree, no releases/, no `current`:
#   local build -> checksum -> upload -> unpack into a staging dir -> verify
#   SHA256SUMS -> install non-secret config -> install the systemd unit ->
#   daemon-reload -> vps-flat-install.sh (per-file atomic rename into
#   /opt/altsignalquality, stale files removed, ExecStart == exe verified).
#
# It DOES NOT start or enable a stopped service (a running one is restarted
# onto the new tree and verified), DOES NOT run database migrations and
# DOES NOT touch NATS: the schema is applied separately (scripts/db-bootstrap.sh
# as postgres) and JetStream by `altsignalquality nats-setup`. Secrets in /etc
# are never overwritten and never printed.
#
# The production filesystem holds exactly one deployment. Old builds are not
# kept for rollback: there is no git history behind them, so a preserved binary
# is an artifact nobody can map back to a source state, and having several of
# them is how a machine ends up running something no one can identify. Restoring
# an earlier implementation is done by rebuilding it from the working copy.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib-vps.sh"

exec 9>/tmp/altsignalquality-deploy.lock
flock -n 9 || { echo "ERROR: another local deploy is already running" >&2; exit 1; }

SKIP_BUILD="${SKIP_BUILD:-0}"
VERSION="$(grep -oP '(?<=project\(altsignalquality VERSION )[0-9.]+' "$REPO_ROOT/CMakeLists.txt")"
RELEASE_BASE="altsignalquality-${VERSION}-linux-x86_64"
ARCHIVE="$REPO_ROOT/dist/${RELEASE_BASE}.tar.zst"

if [[ "$SKIP_BUILD" != "1" ]]; then
    "$SCRIPT_DIR/build-release.sh"
fi
[[ -f "$ARCHIVE" ]] || { echo "ERROR: release archive missing: $ARCHIVE" >&2; exit 1; }

SUM="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
STAMP="$(date -u +%Y%m%d%H%M%S)"
RELEASE_DIR="/tmp/altsignalquality-deploy/stage-${STAMP}"

echo "==> checking free disk on the VPS"
vps_ssh "set -euo pipefail
    free_mb=\$(df --output=avail -m /opt | tail -1 | tr -d ' ')
    if [[ \$free_mb -lt 300 ]]; then
        echo \"ERROR: only \${free_mb} MB free on /opt, 300 MB required\" >&2
        exit 1
    fi
    echo \"    \${free_mb} MB free\""

echo "==> uploading release ($(du -h "$ARCHIVE" | cut -f1))"
vps_ssh "mkdir -p /opt/altsignalquality /tmp/altsignalquality-deploy"
vps_scp "$ARCHIVE" "/tmp/altsignalquality-deploy/${RELEASE_BASE}.tar.zst"
vps_scp "$SCRIPT_DIR/vps-flat-install.sh" "/tmp/altsignalquality-deploy/vps-flat-install.sh"

echo "==> unpacking and verifying on the VPS"
vps_ssh "set -euo pipefail
    echo '${SUM}  /tmp/altsignalquality-deploy/${RELEASE_BASE}.tar.zst' | sha256sum -c - >/dev/null
    stage='${RELEASE_DIR}.staging'
    [[ ! -e '${RELEASE_DIR}' && ! -e \"\$stage\" ]]
    cleanup_stage() { rm -rf -- \"\$stage\"; }
    trap cleanup_stage EXIT
    mkdir -p \"\$stage\"
    tar --use-compress-program=unzstd -xf \
        /tmp/altsignalquality-deploy/${RELEASE_BASE}.tar.zst \
        -C \"\$stage\" --strip-components=1
    cd \"\$stage\" && sha256sum --quiet -c SHA256SUMS
    chown -R root:root \"\$stage\"
    mv \"\$stage\" '${RELEASE_DIR}'
    trap - EXIT
    rm -f /tmp/altsignalquality-deploy/${RELEASE_BASE}.tar.zst"

echo "==> service account and state directories"
vps_ssh "set -euo pipefail
    getent group altsignalquality >/dev/null || groupadd --system altsignalquality
    getent passwd altsignalquality >/dev/null || \
        useradd --system --gid altsignalquality --home-dir /var/lib/altsignalquality \
            --shell /usr/sbin/nologin altsignalquality
    install -d -o altsignalquality -g altsignalquality -m 0750 \
        /var/lib/altsignalquality /var/lib/altsignalquality/models"

echo "==> non-secret configuration in /etc/altsignalquality (never overwritten)"
vps_ssh "set -euo pipefail
    install -d -o root -g altsignalquality -m 0750 /etc/altsignalquality
    src='${RELEASE_DIR}/share/altsignalquality/config-examples'
    [[ -e /etc/altsignalquality/altsignalquality.yaml ]] || \
        install -o root -g altsignalquality -m 0640 \
            \"\$src/altsignalquality.example.yaml\" /etc/altsignalquality/altsignalquality.yaml
    if [[ ! -e /etc/altsignalquality/common.env ]]; then
        install -o root -g altsignalquality -m 0640 \
            \"\$src/common.env.example\" /etc/altsignalquality/common.env
        echo '    WARNING: replace CHANGE_ME in /etc/altsignalquality/common.env'
    fi"

echo "==> systemd unit (installed, NOT started)"
vps_ssh "set -euo pipefail
    install -o root -g root -m 0644 \
        '${RELEASE_DIR}/lib/systemd/system/altsignalquality.service' \
        /etc/systemd/system/altsignalquality.service
    install -d -m 0755 /etc/systemd/system/altsignalquality.service.d
    install -o root -g root -m 0644 \
        '${RELEASE_DIR}/lib/systemd/system/altsignalquality-service-hardening.conf' \
        /etc/systemd/system/altsignalquality.service.d/10-altsignalquality-hardening.conf
    systemctl daemon-reload"

echo "==> installing into /opt/altsignalquality (one tree; restarts only a running service)"
vps_ssh "set -euo pipefail
    bash /tmp/altsignalquality-deploy/vps-flat-install.sh altsignalquality altsignalquality.service \
        bin/altsignalquality '${RELEASE_DIR}' --no-restart"


vps_ssh "rm -rf /tmp/altsignalquality-deploy"
echo "==> deploy OK: /opt/altsignalquality (${VERSION})"
echo "    Before the first start:"
echo "      1) sudo -u postgres ALTSIGNALQUALITY_DB_*=... /opt/altsignalquality/bin/db-bootstrap.sh"
echo "      2) /opt/altsignalquality/bin/altsignalquality nats-setup --config /etc/altsignalquality/altsignalquality.yaml"
echo "      3) systemctl enable --now altsignalquality.service"
