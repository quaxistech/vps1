#!/usr/bin/env bash
# Deploy the AltSignalGateway release to the VPS as an independent service under
# /opt/altmacro -- ONE installed tree, no releases/, no `current`:
#   local build -> checksum -> upload -> unpack into a staging dir -> verify
#   SHA256SUMS -> install non-secret config -> install the systemd unit ->
#   daemon-reload -> vps-flat-install.sh (per-file atomic rename into
#   /opt/altmacro, stale files removed, ExecStart == exe verified; a stopped
#   service is left stopped).
#
# It DOES NOT start or enable the service, DOES NOT run database migrations and
# DOES NOT touch NATS: the schema is applied separately (scripts/db-bootstrap.sh
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

exec 9>/tmp/altmacro-deploy.lock
flock -n 9 || { echo "ERROR: another local deploy is already running" >&2; exit 1; }

SKIP_BUILD="${SKIP_BUILD:-0}"
VERSION="$(grep -oP '(?<=project\(altmacro VERSION )[0-9.]+' "$REPO_ROOT/CMakeLists.txt")"
RELEASE_BASE="altmacro-${VERSION}-linux-x86_64"
ARCHIVE="$REPO_ROOT/dist/${RELEASE_BASE}.tar.zst"

if [[ "$SKIP_BUILD" != "1" ]]; then
    "$SCRIPT_DIR/build-release.sh"
fi
[[ -f "$ARCHIVE" ]] || { echo "ERROR: release archive missing: $ARCHIVE" >&2; exit 1; }

SUM="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
STAMP="$(date -u +%Y%m%d%H%M%S)"
RELEASE_DIR="/tmp/altmacro-deploy/stage-${STAMP}"

echo "==> checking free disk on the VPS"
vps_ssh "set -euo pipefail
    free_mb=\$(df --output=avail -m /opt | tail -1 | tr -d ' ')
    if [[ \$free_mb -lt 300 ]]; then
        echo \"ERROR: only \${free_mb} MB free on /opt, 300 MB required\" >&2
        exit 1
    fi
    echo \"    \${free_mb} MB free\""

echo "==> uploading release ($(du -h "$ARCHIVE" | cut -f1))"
vps_ssh "mkdir -p /opt/altmacro /tmp/altmacro-deploy"
vps_scp "$ARCHIVE" "/tmp/altmacro-deploy/${RELEASE_BASE}.tar.zst"
vps_scp "$SCRIPT_DIR/vps-flat-install.sh" "/tmp/altmacro-deploy/vps-flat-install.sh"

echo "==> unpacking and verifying on the VPS"
vps_ssh "set -euo pipefail
    echo '${SUM}  /tmp/altmacro-deploy/${RELEASE_BASE}.tar.zst' | sha256sum -c - >/dev/null
    stage='${RELEASE_DIR}.staging'
    [[ ! -e '${RELEASE_DIR}' && ! -e \"\$stage\" ]]
    cleanup_stage() { rm -rf -- \"\$stage\"; }
    trap cleanup_stage EXIT
    mkdir -p \"\$stage\"
    tar --use-compress-program=unzstd -xf \
        /tmp/altmacro-deploy/${RELEASE_BASE}.tar.zst \
        -C \"\$stage\" --strip-components=1
    cd \"\$stage\" && sha256sum --quiet -c SHA256SUMS
    chown -R root:root \"\$stage\"
    mv \"\$stage\" '${RELEASE_DIR}'
    trap - EXIT
    rm -f /tmp/altmacro-deploy/${RELEASE_BASE}.tar.zst"

echo "==> service account and state directories"
vps_ssh "set -euo pipefail
    getent group altmacro >/dev/null || groupadd --system altmacro
    getent passwd altmacro >/dev/null || \
        useradd --system --gid altmacro --home-dir /var/lib/altmacro \
            --shell /usr/sbin/nologin altmacro
    install -d -o altmacro -g altmacro -m 0750 \
        /var/lib/altmacro
    # Configuration is root-owned and group-readable: the service reads it and
    # cannot change it."

echo "==> non-secret configuration in /etc/altmacro (never overwritten)"
vps_ssh "set -euo pipefail
    install -d -o root -g altmacro -m 0750 /etc/altmacro
    src='${RELEASE_DIR}/share/altmacro/config-examples'
    [[ -e /etc/altmacro/altmacro.yaml ]] || \
        install -o root -g altmacro -m 0640 \
            \"\$src/altmacro.example.yaml\" /etc/altmacro/altmacro.yaml
    # The runtime secret file. Installed empty as a template: the password is
    # generated on the host and never travels through a deploy log.
    if [[ ! -e /etc/altmacro/altmacro.env ]]; then
        install -o root -g altmacro -m 0640 \
            \"\$src/altmacro.env.example\" /etc/altmacro/altmacro.env
        echo '    NOTE: set ALTMACRO_DB_PASSWORD in /etc/altmacro/altmacro.env'
    fi"

echo "==> systemd unit (installed, NOT started)"
vps_ssh "set -euo pipefail
    install -o root -g root -m 0644 \
        '${RELEASE_DIR}/lib/systemd/system/altmacro.service' \
        /etc/systemd/system/altmacro.service
    install -o root -g root -m 0644 \
        '${RELEASE_DIR}/lib/systemd/system/altmacro-bootstrap.service' \
        /etc/systemd/system/altmacro-bootstrap.service
    systemctl daemon-reload"

echo "==> installing into /opt/altmacro (one tree; restarts only a running service)"
vps_ssh "set -euo pipefail
    bash /tmp/altmacro-deploy/vps-flat-install.sh altmacro altmacro.service \
        bin/altmacro '${RELEASE_DIR}' --no-restart"


vps_ssh "rm -rf /tmp/altmacro-deploy"
echo "==> deploy OK: /opt/altmacro (${VERSION})"
echo "    Before the first start:"
echo "      1) /opt/altmacro/bin/provision.sh"
echo "      2) sudo -u postgres ALTMACRO_DB_*=... /opt/altmacro/bin/db-bootstrap.sh"
echo "      3) altmacro bootstrap --config /etc/altmacro/altmacro.yaml   (hours; resumable)"
echo "      4) systemctl enable --now altmacro"
