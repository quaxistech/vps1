#!/usr/bin/env bash
# Remove everything that was only ever scaffolding, and prove it is gone.
#
# The policy this enforces: one release tree, no rollback copies, no backup
# binaries, and no temporary configuration left where production configuration
# lives. A host whose running version has to be worked out rather than read is a
# host nobody can reason about at three in the morning.
#
#   sudo ./cleanup-host.sh
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "must run as root" >&2; exit 2; }

echo "==> temporary configurations under /etc/altmacro"
for f in smoke.yaml ref.yaml pubtest.yaml altmacro.yaml.bak; do
    if [[ -e "/etc/altmacro/$f" ]]; then rm -f "/etc/altmacro/$f"; echo "    removed $f"; fi
done

echo "==> legacy release layout (one tree under /opt/altmacro now; see vps-flat-install.sh)"
if [[ -L /opt/altmacro/current || -d /opt/altmacro/releases ]]; then
    echo "    /opt/altmacro still has current/ or releases/: run vps-flat-install.sh to migrate" >&2
fi

echo "==> stray artefacts"
rm -rf /tmp/altmacro-deploy /tmp/altmacro_*.log /tmp/altmacro_wide.yaml 2>/dev/null || true

echo "==> nats-server config backups made by nats-user.sh"
# These contain a superseded password each. They served their purpose the moment
# the reload succeeded; leaving them scatters live-shaped credentials around the
# filesystem.
for f in /etc/nats/nats-server.conf.bak.*; do
    [[ -e "$f" ]] || continue
    shred -u "$f" 2>/dev/null || rm -f "$f"
    echo "    removed $(basename "$f")"
done

echo
echo "==> verification"
echo "    releases:            $(ls -1 /opt/altmacro/releases | wc -l)  ($(ls -1 /opt/altmacro/releases))"
echo "    current resolves to: $(basename "$current")"
echo "    /etc/altmacro:       $(ls -1 /etc/altmacro | tr '\n' ' ')"
leftovers="$(ls /etc/altmacro | grep -cE '^(smoke|ref|pubtest)' || true)"
if [[ "$leftovers" -ne 0 ]]; then
    echo "    FAIL: temporary configuration survived"
    exit 1
fi
echo "    no temporary configuration remains"
