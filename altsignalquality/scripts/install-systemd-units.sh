#!/usr/bin/env bash
# systemd unit installation. One service, no job oneshots and no timers:
# training is scheduled by the service itself.
set -euo pipefail

UNIT_SRC="${1:-/opt/altsignalquality/lib/systemd/system}"
[[ "$(id -u)" -eq 0 ]] || { echo "must run as root" >&2; exit 2; }
[[ -d "$UNIT_SRC" ]] || { echo "unit source not found: $UNIT_SRC" >&2; exit 2; }

install -m 0644 "$UNIT_SRC/altsignalquality.service" /etc/systemd/system/
install -d -m 0755 /etc/systemd/system/altsignalquality.service.d
install -m 0644 "$UNIT_SRC/altsignalquality-service-hardening.conf" \
    /etc/systemd/system/altsignalquality.service.d/10-altsignalquality-hardening.conf

systemctl daemon-reload
systemctl enable altsignalquality.service >/dev/null

echo "AltSignalQuality systemd unit installed (service NOT started)"
