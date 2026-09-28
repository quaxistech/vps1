#!/usr/bin/env bash
# Add or update the AltSignalGateway account in the shared NATS server config.
#
# The NATS configuration file belongs to AltSpotter, which owns the broker. This
# script edits ONE block in it — the authorization user for this service — and
# leaves every other account untouched. It backs the file up first and reloads
# the server rather than restarting it, so no other service loses its
# connection.
#
# LEAST PRIVILEGE. The account below can:
#   subscribe  signalquality.final.>   the FINAL signals it journals
#              _INBOX.>                its own request/reply inboxes
#   publish    $JS.API.>, $JS.ACK.>    bind its durable and acknowledge
#              _INBOX.>
#
# It has NO publish permission on any subject that carries data. This service
# produces nothing on NATS: its output is the WebSocket. It also cannot read
# `signal.>` (AltStrategy's proposals), `alttech.>`, `prediction.>` or any
# market-data subject — the FINAL signal is the only thing it is entitled to.
set -euo pipefail

: "${ALTSIGNALGATEWAY_NATS_USER:?set ALTSIGNALGATEWAY_NATS_USER}"
: "${ALTSIGNALGATEWAY_NATS_PASSWORD:?set ALTSIGNALGATEWAY_NATS_PASSWORD}"
CONF="${NATS_CONF:-/etc/nats/nats-server.conf}"

[[ -f "$CONF" ]] || { echo "NATS config not found: $CONF" >&2; exit 2; }
[[ $EUID -eq 0 ]] || { echo "must run as root: $CONF is root-owned" >&2; exit 2; }

if grep -q "user: *\"\?${ALTSIGNALGATEWAY_NATS_USER}\"\?" "$CONF"; then
    echo "account ${ALTSIGNALGATEWAY_NATS_USER} already present in $CONF"
    echo "  (rotate the password by editing the file directly, then: nats-server --signal reload)"
    exit 0
fi

BACKUP="${CONF}.bak-$(date -u +%Y%m%d%H%M%S)"
cp -a "$CONF" "$BACKUP"
echo "backed up $CONF -> $BACKUP"

# The insertion point is the line that closes the `users: [ ... ]` array. It is
# located with POSIX character classes, never `\s`: on Ubuntu `awk` is mawk,
# where `\s` is not a regex escape and silently matches a literal 's'.
USERS_LINE=$(grep -nE '^[[:space:]]*users:[[:space:]]*\[' "$CONF" | head -1 | cut -d: -f1)
[[ -n "$USERS_LINE" ]] || { echo "ERROR: no users array in $CONF" >&2; exit 1; }
CLOSE_LINE=$(awk -v start="$USERS_LINE" 'NR > start && /^[[:space:]]*\][[:space:]]*$/ { print NR; exit }' "$CONF")
[[ -n "$CLOSE_LINE" ]] || { echo "ERROR: could not find the end of the users array" >&2; exit 1; }

# The last existing entry has no trailing comma, so one is added before the new
# block rather than after it.
LAST_ENTRY=$(awk -v s="$USERS_LINE" -v e="$CLOSE_LINE" \
    'NR > s && NR < e && $0 !~ /^[[:space:]]*(#|$)/ { last = NR } END { print last }' "$CONF")

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
{
    head -n "$((LAST_ENTRY - 1))" "$CONF"
    # The separating comma is appended to the previous entry rather than put on
    # a line of its own: the NATS parser rejects a bare `,` between array
    # elements ("Unexpected array value terminator").
    sed -n "${LAST_ENTRY}p" "$CONF" | sed 's/[[:space:]]*$/,/'
    echo "        # AltSignalGateway consumes the FINAL TradeSignal and owns NOTHING on"
    echo "        # the bus: its output is a TLS WebSocket, not a subject. It can bind"
    echo "        # its own durable and acknowledge, and read nothing else."
    echo "        { user: \"${ALTSIGNALGATEWAY_NATS_USER}\", password: \"${ALTSIGNALGATEWAY_NATS_PASSWORD}\","
    echo "          permissions: {"
    printf '            publish:   { allow: ["%s", "%s", "%s"] },\n' '$JS.API.>' '$JS.ACK.>' '_INBOX.>'
    printf '            subscribe: { allow: ["%s", "%s"] } } }\n' 'signalquality.final.>' '_INBOX.>'
    tail -n +"$((LAST_ENTRY + 1))" "$CONF"
} > "$TMP"

if ! grep -q "${ALTSIGNALGATEWAY_NATS_USER}" "$TMP"; then
    echo "ERROR: the edit did not take; $CONF is unchanged" >&2
    exit 1
fi
# A broker that will not parse its own configuration takes the whole stack down,
# so the edited copy is validated BEFORE it replaces the original.
if command -v nats-server >/dev/null 2>&1; then
    nats-server -t -c "$TMP" >/dev/null || {
        echo "ERROR: the edited configuration does not parse; $CONF is unchanged" >&2
        exit 1
    }
fi

cat "$TMP" > "$CONF"
echo "added ${ALTSIGNALGATEWAY_NATS_USER} to $CONF"

if systemctl is-active --quiet nats-server; then
    # Reload, never restart: a restart drops every other service's connection.
    systemctl reload nats-server || nats-server --signal reload
    echo "reloaded nats-server"
fi
