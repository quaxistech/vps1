#!/usr/bin/env bash
# What ships, and what must never ship.
#
# The rules below are the ones a code review reliably misses: a secret that
# slipped into a config example, a TODO left on the production path, a document
# the installer promises but does not contain.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
rc=0
fails=0
fail() { echo "FAIL  $1"; rc=1; fails=$((fails + 1)); }
ok()   { echo "OK    $1"; }
# Each block records the failure count it started with, so a later block cannot
# report OK just because an earlier one had already set rc.
mark()  { before=$fails; }
done_() { [[ $fails -eq $before ]] && ok "$1"; return 0; }

# --- no secrets in the tree ---------------------------------------------------
# vps.md and *.env are gitignored; everything else is fair game for a scan.
SCAN_PATHS=(config db deploy docs proto scripts src CMakeLists.txt README.md)
mark
# A 32+ byte hex literal in a shipped file is either a key or something that
# looks exactly like one.
if grep -rInE '(secret|password|passwd|api[_-]?key)[[:space:]]*[:=][[:space:]]*"?[0-9a-fA-F]{32,}' \
        "${SCAN_PATHS[@]}" 2>/dev/null | grep -v 'CHANGE_ME' | grep -v 'tests/unit'; then
    fail "a literal secret appears in a shipped file"
fi
# Placeholders must be env references or CHANGE_ME, never a real-looking value.
if grep -rIn 'secret_hex:' config/ 2>/dev/null | grep -v '${'; then
    fail "config/ contains a secret_hex that is not an environment reference"
fi
if grep -rIn -- '-----BEGIN .*PRIVATE KEY-----' "${SCAN_PATHS[@]}" 2>/dev/null; then
    fail "a private key is committed"
fi
done_ "no secret material in the shipped tree"

# --- no unfinished work on the production path --------------------------------
mark
if grep -rInE '(TODO|FIXME|XXX|HACK)' src/ proto/ db/ 2>/dev/null; then
    fail "an unfinished marker remains on the production path"
fi
# A promised behaviour that does not exist. Comment lines are stripped first:
# the code legitimately DISCUSSES placeholders it refuses to emit.
if grep -rInE 'not implemented|unimplemented|\bstub\b' src/ 2>/dev/null \
        | grep -vE ':[0-9]+: *(//|\*|/\*)'; then
    fail "a stub remains in src/"
fi
done_ "no TODO, FIXME or stub on the production path"

# --- the installer must not promise a document that does not exist ------------
mark
for doc in $(grep -oE 'docs/[A-Z_]+\.md' CMakeLists.txt | sort -u); do
    [[ -f "$doc" ]] || fail "CMakeLists.txt installs a missing document: $doc"
done
for f in README.md docs/CONSUMER_GUIDE.md docs/ARCHITECTURE.md docs/SIGNAL_CONTRACT.md docs/WEBSOCKET_PROTOCOL.md \
         docs/JOURNAL.md docs/DELIVERY.md docs/SECURITY.md \
         docs/OPERATIONS.md docs/TESTING.md docs/CONFIGURATION.md; do
    [[ -f "$f" ]] || fail "required document missing: $f"
done
done_ "every installed document exists"

# --- the installer must not promise a script that does not exist --------------
mark
for s in $(grep -oE 'scripts/[a-z-]+\.sh' CMakeLists.txt | sort -u); do
    [[ -f "$s" ]] || fail "CMakeLists.txt installs a missing script: $s"
    [[ -x "$s" ]] || fail "installed script is not executable: $s"
done
done_ "every installed script exists and is executable"

# --- shell scripts must at least parse ----------------------------------------
mark
for s in scripts/*.sh tests/shell/*.sh; do
    bash -n "$s" || fail "shell syntax error in $s"
done
done_ "every shell script parses"

# --- the systemd unit must be a production unit -------------------------------
UNIT=deploy/systemd/altsignalgateway.service
HARDENING=deploy/systemd/altsignalgateway-service-hardening.conf
mark
grep -q '^Type=notify' "$UNIT" || fail "the unit is not Type=notify"
grep -q '^User=altsignalgateway' "$UNIT" || fail "the unit does not run as its own user"
grep -q '^WatchdogSec=' "$UNIT" || fail "no watchdog is armed"
grep -q '^Restart=on-failure' "$UNIT" || fail "no restart policy"
grep -q '^KillSignal=SIGTERM' "$UNIT" || fail "graceful termination is not configured"
grep -q '^MemoryMax=' "$UNIT" || fail "no memory bound"
grep -q '^NoNewPrivileges=true' "$HARDENING" || fail "NoNewPrivileges is not set"
grep -q '^ProtectSystem=strict' "$HARDENING" || fail "ProtectSystem is not strict"
grep -q '^CapabilityBoundingSet=$' "$HARDENING" || fail "capabilities are not dropped"
grep -q '^LimitCORE=0' "$HARDENING" || fail "core dumps are not disabled (they contain key material)"
done_ "the systemd unit is hardened"

# --- the endpoint must have no plaintext mode ---------------------------------
mark
if grep -rInE '^\s*enabled:' config/altsignalgateway.example.yaml | grep -q tls; then
    fail "the example config exposes a TLS on/off switch"
fi
grep -q 'no plaintext mode' src/altsignalgateway/core/config.cpp \
    || fail "config.cpp does not refuse a missing certificate"
done_ "the WebSocket endpoint has no plaintext mode"

[[ $rc -eq 0 ]] && echo "release contract: OK" || echo "release contract: FAILED"
exit $rc
