#!/usr/bin/env bash
set -euo pipefail

UNIT_SRC="${1:-/opt/altml/lib/systemd/system}"
[[ "$(id -u)" -eq 0 ]] || { echo "must run as root" >&2; exit 2; }
[[ -d "$UNIT_SRC" ]] || { echo "unit source not found: $UNIT_SRC" >&2; exit 2; }

# Retired units: the pre-unification per-component daemons AND the Python-era
# job oneshots/timers. The internal native trainer replaced the timers; running
# both would double-train, so the old units are removed, not just disabled.
retired=(
    altml-nats-setup.service
    altml-label-builder.service
    altml-model-registry.service
    altml-inference.service
    altml-dataset-build.service
    altml-dataset-build.timer
    altml-training.service
    altml-training.timer
)
for unit in "${retired[@]}"; do
    systemctl disable --now "$unit" >/dev/null 2>&1 || true
    rm -f "/etc/systemd/system/$unit"
    rm -rf "/etc/systemd/system/$unit.d"
done
systemctl disable --now altml.target >/dev/null 2>&1 || true
rm -f /etc/systemd/system/altml.target

install -m 0644 "$UNIT_SRC/altml.service" /etc/systemd/system/
install -d -m 0755 /etc/systemd/system/altml.service.d
install -m 0644 "$UNIT_SRC/altml-service-hardening.conf" \
    /etc/systemd/system/altml.service.d/10-altml-hardening.conf

systemctl daemon-reload
systemctl enable altml.service >/dev/null

echo "AltML systemd units installed (single service; no job timers)"
