#!/usr/bin/env bash
set -euo pipefail

CONF_SRC="${1:-/opt/altml/share/altml/config-examples}"
[[ "$(id -u)" -eq 0 ]] || { echo "must run as root" >&2; exit 2; }
[[ -d "$CONF_SRC" ]] || { echo "config examples not found: $CONF_SRC" >&2; exit 2; }

getent group altml >/dev/null || groupadd --system altml
getent passwd altml >/dev/null || \
    useradd --system --gid altml --home-dir /var/lib/altml \
        --shell /usr/sbin/nologin altml

install -d -m 0755 /opt/altml /opt/altml/releases
install -d -o root -g altml -m 0750 /etc/altml
install -d -o altml -g altml -m 0750 \
    /var/lib/altml /var/lib/altml/datasets /var/lib/altml/models

[[ -e /etc/altml/altml.yaml ]] || install -o root -g altml -m 0640 \
    "$CONF_SRC/altml.example.yaml" /etc/altml/altml.yaml

if [[ ! -e /etc/altml/common.env ]]; then
    install -o root -g altml -m 0640 \
        "$CONF_SRC/common.env.example" /etc/altml/common.env
    echo "WARNING: replace CHANGE_ME values in /etc/altml/common.env before start" >&2
fi

echo "AltML filesystem and service account provisioned"
echo "Run scripts/db-bootstrap.sh (as postgres) to create the altml database, its FDW"
echo "access to AltSpotter/AltTech, and apply db/schema.sql before first start."
