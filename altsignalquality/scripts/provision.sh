#!/usr/bin/env bash
# Service account and filesystem layout. Idempotent.
set -euo pipefail

CONF_SRC="${1:-/opt/altsignalquality/share/altsignalquality/config-examples}"
[[ "$(id -u)" -eq 0 ]] || { echo "must run as root" >&2; exit 2; }
[[ -d "$CONF_SRC" ]] || { echo "config examples not found: $CONF_SRC" >&2; exit 2; }

getent group altsignalquality >/dev/null || groupadd --system altsignalquality
getent passwd altsignalquality >/dev/null || \
    useradd --system --gid altsignalquality --home-dir /var/lib/altsignalquality \
        --shell /usr/sbin/nologin altsignalquality

install -d -m 0755 /opt/altsignalquality /opt/altsignalquality/releases
install -d -o root -g altsignalquality -m 0750 /etc/altsignalquality
install -d -o altsignalquality -g altsignalquality -m 0750 \
    /var/lib/altsignalquality /var/lib/altsignalquality/models

[[ -e /etc/altsignalquality/altsignalquality.yaml ]] || \
    install -o root -g altsignalquality -m 0640 \
        "$CONF_SRC/altsignalquality.example.yaml" \
        /etc/altsignalquality/altsignalquality.yaml

if [[ ! -e /etc/altsignalquality/common.env ]]; then
    install -o root -g altsignalquality -m 0640 \
        "$CONF_SRC/common.env.example" /etc/altsignalquality/common.env
    echo "WARNING: replace CHANGE_ME in /etc/altsignalquality/common.env before start" >&2
fi

echo "AltSignalQuality filesystem and service account provisioned"
echo "Next: scripts/db-bootstrap.sh (as postgres) creates the role and schema"
echo "inside the AltSpotter database."
