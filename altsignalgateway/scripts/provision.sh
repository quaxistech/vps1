#!/usr/bin/env bash
# Prepare the host for AltSignalGateway: service account, state and config
# directories, permissions.
#
# Idempotent. It creates no secrets and overwrites no existing configuration —
# a re-run on a live box changes nothing that is already correct.
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "must run as root" >&2; exit 2; }

echo "==> service account"
getent group altsignalgateway >/dev/null || groupadd --system altsignalgateway
getent passwd altsignalgateway >/dev/null || \
    useradd --system --gid altsignalgateway --home-dir /var/lib/altsignalgateway \
        --shell /usr/sbin/nologin altsignalgateway

echo "==> directories"
# Runtime state is deliberately NOT next to the executable: /opt is made
# read-only by the deploy and by ProtectSystem=strict.
install -d -o altsignalgateway -g altsignalgateway -m 0750 /var/lib/altsignalgateway
# Configuration is root-owned and group-readable: the service reads it, the
# service cannot change it.
install -d -o root -g altsignalgateway -m 0750 /etc/altsignalgateway
# TLS material is stricter still: 0700 on the directory, 0640/0600 on the files.
install -d -o root -g altsignalgateway -m 0750 /etc/altsignalgateway/tls

echo "==> permissions on any existing secrets"
for f in /etc/altsignalgateway/common.env /etc/altsignalgateway/clients.yaml; do
    [[ -e "$f" ]] && chown root:altsignalgateway "$f" && chmod 0640 "$f"
done
if [[ -e /etc/altsignalgateway/tls/server.key ]]; then
    chown root:altsignalgateway /etc/altsignalgateway/tls/server.key
    chmod 0640 /etc/altsignalgateway/tls/server.key
fi
if [[ -e /etc/altsignalgateway/tls/server.crt ]]; then
    chown root:altsignalgateway /etc/altsignalgateway/tls/server.crt
    chmod 0644 /etc/altsignalgateway/tls/server.crt
fi

echo "host is provisioned for altsignalgateway"
