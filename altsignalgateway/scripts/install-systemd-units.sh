#!/usr/bin/env bash
# Install the AltSignalGateway systemd unit and its hardening drop-in.
#
# Installs, never starts: the database schema, the JetStream durable, the TLS
# material and the client credentials all have to exist first, and a unit that
# starts before them just burns its restart budget.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -d "$SCRIPT_DIR/../deploy/systemd" ]]; then
    UNIT_DIR="$SCRIPT_DIR/../deploy/systemd"
else
    UNIT_DIR="$SCRIPT_DIR/../lib/systemd/system"
fi
[[ -d "$UNIT_DIR" ]] || { echo "unit directory not found: $UNIT_DIR" >&2; exit 2; }
[[ $EUID -eq 0 ]] || { echo "must run as root" >&2; exit 2; }

install -o root -g root -m 0644 \
    "$UNIT_DIR/altsignalgateway.service" /etc/systemd/system/altsignalgateway.service
install -d -m 0755 /etc/systemd/system/altsignalgateway.service.d
install -o root -g root -m 0644 \
    "$UNIT_DIR/altsignalgateway-service-hardening.conf" \
    /etc/systemd/system/altsignalgateway.service.d/10-altsignalgateway-hardening.conf
systemctl daemon-reload

echo "installed altsignalgateway.service (not started)"
echo "Before the first start:"
echo "  1) db-bootstrap.sh as postgres"
echo "  2) nats-user.sh as root, then set ALTSIGNALGATEWAY_NATS_URL"
echo "  3) gen-tls-cert.sh (or install a real certificate) into /etc/altsignalgateway/tls"
echo "  4) fill /etc/altsignalgateway/clients.yaml and common.env"
echo "  5) altsignalgateway validate --config /etc/altsignalgateway/altsignalgateway.yaml"
echo "  6) altsignalgateway nats-setup --config /etc/altsignalgateway/altsignalgateway.yaml"
echo "  7) systemctl enable --now altsignalgateway.service"
