#!/usr/bin/env bash
# Add altmacro's NATS user with least-privilege permissions and record its URL
# in the runtime secret file.
#
# altmacro publishes and subscribes to nothing: it owns one stream and no
# consumers, because downstream services declare their own durables. The
# permissions below say exactly that, so a bug here cannot read another
# service's traffic or disturb its cursors.
#
# The password is generated on this host, written straight into the root-owned
# env file and the server config, and never printed.
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "must run as root" >&2; exit 2; }

CONF="${NATS_CONF:-/etc/nats/nats-server.conf}"
ENVFILE=/etc/altmacro/altmacro.env
HOSTPORT="${NATS_HOSTPORT:-127.0.0.1:4222}"

[[ -f "$CONF" ]] || { echo "nats config not found: $CONF" >&2; exit 2; }

if grep -q 'user: *"\?altmacro"\?' "$CONF"; then
    echo "the altmacro NATS user already exists; leaving the server config alone"
else
    PW="$(head -c 32 /dev/urandom | base64 | tr -d '/+=' | head -c 32)"
    cp -a "$CONF" "$CONF.bak.$(date -u +%Y%m%d%H%M%S)"
    python3 - "$CONF" "$PW" <<'PY'
import sys
conf, pw = sys.argv[1], sys.argv[2]
s = open(conf).read()
block = (
    '        # altmacro publishes external market and macro reference series and\n'
    '        # owns nothing else on the bus: one stream, no consumers, no reads of\n'
    '        # any other service\'s subjects.\n'
    '        { user: "altmacro", password: "%s",\n'
    '          permissions: {\n'
    '            publish:   { allow: ["altmacro.>", "$JS.API.>", "_INBOX.>"] },\n'
    '            subscribe: { allow: ["_INBOX.>"] } } }\n' % pw)
marker = s.rindex(']')
# Keep the list well-formed: the previous entry needs a separating comma.
head = s[:marker].rstrip()
if not head.endswith(','):
    head += ','
open(conf, 'w').write(head + '\n' + block + '    ' + s[marker:])
PY
    umask 027
    tmp="$(mktemp)"
    grep -v '^ALTMACRO_NATS_URL=' "$ENVFILE" 2>/dev/null > "$tmp" || true
    printf 'ALTMACRO_NATS_URL=nats://altmacro:%s@%s\n' "$PW" "$HOSTPORT" >> "$tmp"
    install -o root -g altmacro -m 0640 "$tmp" "$ENVFILE"
    rm -f "$tmp"
    unset PW
    echo "altmacro NATS user created; URL written to $ENVFILE"
fi

echo "==> reloading nats-server"
systemctl reload nats-server 2>/dev/null || systemctl restart nats-server
sleep 2
systemctl is-active nats-server
