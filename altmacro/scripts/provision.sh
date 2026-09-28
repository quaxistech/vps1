#!/usr/bin/env bash
# Prepare the host for AltSignalGateway: service account, state and config
# directories, permissions.
#
# Idempotent. It creates no secrets and overwrites no existing configuration —
# a re-run on a live box changes nothing that is already correct.
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "must run as root" >&2; exit 2; }

echo "==> service account"
getent group altmacro >/dev/null || groupadd --system altmacro
getent passwd altmacro >/dev/null || \
    useradd --system --gid altmacro --home-dir /var/lib/altmacro \
        --shell /usr/sbin/nologin altmacro

echo "==> directories"
# Runtime state is deliberately NOT next to the executable: /opt is made
# read-only by the deploy and by ProtectSystem=strict.
install -d -o altmacro -g altmacro -m 0750 /var/lib/altmacro
# Configuration is root-owned and group-readable: the service reads it, the
# service cannot change it.
install -d -o root -g altmacro -m 0750 /etc/altmacro
# TLS material is stricter still: 0700 on the directory, 0640/0600 on the files.

echo "==> permissions on the runtime secret file"
# The database password lives here and nowhere else: not in the YAML, not in
# the binary, not in any log line.
if [[ -e /etc/altmacro/altmacro.env ]]; then
    chown root:altmacro /etc/altmacro/altmacro.env
    chmod 0640 /etc/altmacro/altmacro.env
fi

echo "host is provisioned for altmacro"
