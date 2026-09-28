#!/usr/bin/env bash
# Shared helpers: read the VPS connection details from vps.md WITHOUT echoing
# secrets, and run ssh/scp with the pinned deploy key. Sourced by deploy.sh.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Use the configured target by default. Set VPS_FILE to select another target.
VPS_FILE="${VPS_FILE:-$REPO_ROOT/vps1.md}"
# The stack's pinned deploy key is the default.
SSH_KEY="${ALTSIGNALQUALITY_SSH_KEY:-$HOME/.ssh/altstrategy_vps_ed25519}"

if [[ ! -f "$VPS_FILE" ]]; then
    echo "ERROR: vps1.md not found at $VPS_FILE" >&2
    exit 2
fi

# The target file uses the Russian labels below.
VPS_HOST="$(grep -oP '(?<=IP-адрес сервера: ).*' "$VPS_FILE" | tr -d '[:space:]')"
VPS_USER="$(grep -oP '(?<=Пользователь: ).*' "$VPS_FILE" | tr -d '[:space:]')"
VPS_PORT="${VPS_PORT:-22}"

if [[ -z "$VPS_HOST" || -z "$VPS_USER" ]]; then
    echo "ERROR: could not parse host/user from vps1.md" >&2
    exit 2
fi
if [[ ! -r "$SSH_KEY" ]]; then
    echo "ERROR: the VPS SSH key is not readable: $SSH_KEY" >&2
    exit 2
fi

# accept-new pins the host key on first contact and refuses a later change: a
# VPS replacement or a MITM must be investigated before deploying again.
SSH_OPTS=(-o StrictHostKeyChecking=accept-new -o ConnectTimeout=15 -p "$VPS_PORT"
          -i "$SSH_KEY" -o IdentitiesOnly=yes -o BatchMode=yes)
SCP_OPTS=(-o StrictHostKeyChecking=accept-new -o ConnectTimeout=15 -P "$VPS_PORT"
          -i "$SSH_KEY" -o IdentitiesOnly=yes -o BatchMode=yes)

vps_ssh() { ssh "${SSH_OPTS[@]}" "$VPS_USER@$VPS_HOST" "$@"; }
vps_scp() { scp "${SCP_OPTS[@]}" "$1" "$VPS_USER@$VPS_HOST:$2"; }
