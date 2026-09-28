#!/usr/bin/env bash
# Deploy the AltSignalGateway release to the VPS as an independent service under
# /opt/altsignalgateway -- ONE installed tree, no releases/, no `current`:
#   local build -> checksum -> upload -> unpack into a staging dir -> verify
#   SHA256SUMS -> install non-secret config -> install the systemd unit ->
#   daemon-reload -> vps-flat-install.sh (per-file atomic rename into
#   /opt/altsignalgateway, stale files removed, ExecStart == exe verified;
#   a stopped service is left stopped).
#
# It DOES NOT start or enable the service, DOES NOT run database migrations and
# DOES NOT touch NATS: the schema is applied separately (scripts/db-bootstrap.sh
# as postgres) and JetStream by `altsignalgateway nats-setup`. Secrets in /etc
# are never overwritten and never printed.
#
# The production filesystem holds exactly one deployment. Old builds are not
# kept for rollback: with no git history behind them a retained binary cannot be
# mapped back to a source state, so it is an artifact nobody can identify, and a
# host holding several is one whose running version has to be worked out rather
# than read. An earlier implementation is restored by rebuilding it.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib-vps.sh"

exec 9>/tmp/altsignalgateway-deploy.lock
flock -n 9 || { echo "ERROR: another local deploy is already running" >&2; exit 1; }

SKIP_BUILD="${SKIP_BUILD:-0}"
VERSION="$(grep -oP '(?<=project\(altsignalgateway VERSION )[0-9.]+' "$REPO_ROOT/CMakeLists.txt")"
RELEASE_BASE="altsignalgateway-${VERSION}-linux-x86_64"
ARCHIVE="$REPO_ROOT/dist/${RELEASE_BASE}.tar.zst"

if [[ "$SKIP_BUILD" != "1" ]]; then
    "$SCRIPT_DIR/build-release.sh"
fi
[[ -f "$ARCHIVE" ]] || { echo "ERROR: release archive missing: $ARCHIVE" >&2; exit 1; }

SUM="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
STAMP="$(date -u +%Y%m%d%H%M%S)"
RELEASE_DIR="/tmp/altsignalgateway-deploy/stage-${STAMP}"

echo "==> checking free disk on the VPS"
vps_ssh "set -euo pipefail
    free_mb=\$(df --output=avail -m /opt | tail -1 | tr -d ' ')
    if [[ \$free_mb -lt 300 ]]; then
        echo \"ERROR: only \${free_mb} MB free on /opt, 300 MB required\" >&2
        exit 1
    fi
    echo \"    \${free_mb} MB free\""

echo "==> uploading release ($(du -h "$ARCHIVE" | cut -f1))"
vps_ssh "mkdir -p /opt/altsignalgateway /tmp/altsignalgateway-deploy"
vps_scp "$ARCHIVE" "/tmp/altsignalgateway-deploy/${RELEASE_BASE}.tar.zst"
vps_scp "$SCRIPT_DIR/vps-flat-install.sh" "/tmp/altsignalgateway-deploy/vps-flat-install.sh"

echo "==> unpacking and verifying on the VPS"
vps_ssh "set -euo pipefail
    echo '${SUM}  /tmp/altsignalgateway-deploy/${RELEASE_BASE}.tar.zst' | sha256sum -c - >/dev/null
    stage='${RELEASE_DIR}.staging'
    [[ ! -e '${RELEASE_DIR}' && ! -e \"\$stage\" ]]
    cleanup_stage() { rm -rf -- \"\$stage\"; }
    trap cleanup_stage EXIT
    mkdir -p \"\$stage\"
    tar --use-compress-program=unzstd -xf \
        /tmp/altsignalgateway-deploy/${RELEASE_BASE}.tar.zst \
        -C \"\$stage\" --strip-components=1
    cd \"\$stage\" && sha256sum --quiet -c SHA256SUMS
    chown -R root:root \"\$stage\"
    mv \"\$stage\" '${RELEASE_DIR}'
    trap - EXIT
    rm -f /tmp/altsignalgateway-deploy/${RELEASE_BASE}.tar.zst"

echo "==> service account and state directories"
vps_ssh "set -euo pipefail
    getent group altsignalgateway >/dev/null || groupadd --system altsignalgateway
    getent passwd altsignalgateway >/dev/null || \
        useradd --system --gid altsignalgateway --home-dir /var/lib/altsignalgateway \
            --shell /usr/sbin/nologin altsignalgateway
    install -d -o altsignalgateway -g altsignalgateway -m 0750 \
        /var/lib/altsignalgateway
    # Configuration is root-owned and group-readable: the service reads it and
    # cannot change it. TLS material lives in its own directory.
    install -d -o root -g altsignalgateway -m 0750 /etc/altsignalgateway/tls"

echo "==> non-secret configuration in /etc/altsignalgateway (never overwritten)"
vps_ssh "set -euo pipefail
    install -d -o root -g altsignalgateway -m 0750 /etc/altsignalgateway
    src='${RELEASE_DIR}/share/altsignalgateway/config-examples'
    [[ -e /etc/altsignalgateway/altsignalgateway.yaml ]] || \
        install -o root -g altsignalgateway -m 0640 \
            \"\$src/altsignalgateway.example.yaml\" /etc/altsignalgateway/altsignalgateway.yaml
    # The credentials file holds the WebSocket client keys. It is installed as a
    # template with env references only, never with key material.
    if [[ ! -e /etc/altsignalgateway/clients.yaml ]]; then
        install -o root -g altsignalgateway -m 0640 \
            \"\$src/clients.example.yaml\" /etc/altsignalgateway/clients.yaml
        echo '    WARNING: set the client secrets in /etc/altsignalgateway/common.env'
    fi
    if [[ ! -e /etc/altsignalgateway/common.env ]]; then
        install -o root -g altsignalgateway -m 0640 \
            \"\$src/common.env.example\" /etc/altsignalgateway/common.env
        echo '    WARNING: replace CHANGE_ME in /etc/altsignalgateway/common.env'
    fi"

echo "==> systemd unit (installed, NOT started)"
vps_ssh "set -euo pipefail
    install -o root -g root -m 0644 \
        '${RELEASE_DIR}/lib/systemd/system/altsignalgateway.service' \
        /etc/systemd/system/altsignalgateway.service
    install -d -m 0755 /etc/systemd/system/altsignalgateway.service.d
    install -o root -g root -m 0644 \
        '${RELEASE_DIR}/lib/systemd/system/altsignalgateway-service-hardening.conf' \
        /etc/systemd/system/altsignalgateway.service.d/10-altsignalgateway-hardening.conf
    systemctl daemon-reload"

echo "==> installing into /opt/altsignalgateway (one tree; restarts only a running service)"
vps_ssh "set -euo pipefail
    bash /tmp/altsignalgateway-deploy/vps-flat-install.sh altsignalgateway altsignalgateway.service \
        bin/altsignalgateway '${RELEASE_DIR}' --no-restart"


vps_ssh "rm -rf /tmp/altsignalgateway-deploy"
echo "==> deploy OK: /opt/altsignalgateway (${VERSION})"
echo "    Before the first start:"
echo "      1) /opt/altsignalgateway/bin/provision.sh"
echo "      2) sudo -u postgres ALTSIGNALGATEWAY_DB_*=... /opt/altsignalgateway/bin/db-bootstrap.sh"
echo "      3) ALTSIGNALGATEWAY_NATS_* =... /opt/altsignalgateway/bin/nats-user.sh"
echo "      4) /opt/altsignalgateway/bin/gen-tls-cert.sh /etc/altsignalgateway/tls   (or install a real certificate)"
echo "      5) fill /etc/altsignalgateway/common.env and clients.yaml"
echo "      6) /opt/altsignalgateway/bin/altsignalgateway validate --config /etc/altsignalgateway/altsignalgateway.yaml"
echo "      7) /opt/altsignalgateway/bin/altsignalgateway nats-setup --config /etc/altsignalgateway/altsignalgateway.yaml"
echo "      8) systemctl enable --now altsignalgateway.service"
