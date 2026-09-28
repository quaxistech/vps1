#!/usr/bin/env bash
# The restart / resume scenario, run against the DEPLOYED service.
#
#   client ACKs #N
#   disconnect
#   Gateway restart (and, in the second half, SIGKILL)
#   client reconnects, RESUME(last_acked = N)
#   Gateway sends N+1, N+2, ...
#   client ACKs them
#
# The result must be deterministic: the same signals, in order, once each, with
# the same payload hashes across the restart.
set -uo pipefail

VPS="${ALTSGW_HOST:?set ALTSGW_HOST=user@host}"
SSH_KEY="${ALTSIGNALGATEWAY_SSH_KEY:-$HOME/.ssh/altstrategy_vps_ed25519}"
BIN=/opt/altsignalgateway/bin/altsignalgateway
CERT=/etc/altsignalgateway/tls/server.crt

rc=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; rc=1; }

r() { ssh -o BatchMode=yes -i "$SSH_KEY" "$VPS" "$@"; }
psql_() { r "sudo -u postgres psql -d altspotter -tAc \"$1\""; }
client() {
    r "set -a; . /etc/altsignalgateway/common.env; set +a; timeout ${CLIENT_TIMEOUT:-40} $BIN client \
        --host 127.0.0.1 --port 9443 --sni altsignalgateway --ca-file $CERT $*"
}
# The observer is used wherever the test only needs to READ the stream, so a
# test run can never move the executor's production cursor.
OBS="--client-id vps1-observer --key-id vps1-observer-k1 --secret-env ALTSIGNALGATEWAY_CLIENT_OBSERVER_SECRET"
EXEC="--client-id vps2-executor --key-id vps2-executor-k1 --secret-env ALTSIGNALGATEWAY_CLIENT_VPS2_SECRET"

echo "=== restart / resume — $(date -u +%FT%TZ) ==="

NOW_MS=$(r "date +%s%3N")
LIVE=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal
    WHERE expires_at_ms > $NOW_MS AND delivery_state NOT IN ('EXPIRED','FAILED')")
echo "    non-expired signals in the journal: $LIVE"
if [[ "$LIVE" -lt 3 ]]; then
    echo "SKIP  fewer than 3 non-expired signals; the scenario needs three to be meaningful"
    exit 0
fi

# The three sequences the scenario walks through — chosen ABOVE the observer's
# own durable cursor. A resume request below that cursor is clamped to it (which
# is the point of the cursor), so a scenario that ignored it would be asking the
# server to do something it correctly refuses.
OBS_CURSOR=$(psql_ "SELECT COALESCE(last_acked_sequence,0) FROM altsignalgateway.client_cursors
                     WHERE client_id='vps1-observer'")
OBS_CURSOR=${OBS_CURSOR:-0}
read -r S1 S2 S3 <<<"$(psql_ "SELECT string_agg(sequence::text,' ' ORDER BY sequence) FROM (
    SELECT sequence FROM altsignalgateway.signal_journal
     WHERE expires_at_ms > $NOW_MS AND delivery_state NOT IN ('EXPIRED','FAILED')
       AND sequence > $OBS_CURSOR
     ORDER BY sequence LIMIT 3) t")"
if [[ -z "${S3:-}" ]]; then
    echo "SKIP  fewer than 3 non-expired signals above the observer cursor ($OBS_CURSOR)"
    exit 0
fi
echo "    observer cursor=$OBS_CURSOR; using sequences $S1, $S2, $S3"

# Payload hashes before the restart, to compare afterwards.
HASH_BEFORE=$(psql_ "SELECT string_agg(payload_sha256, ',' ORDER BY sequence)
    FROM altsignalgateway.signal_journal WHERE sequence IN ($S1,$S2,$S3)")

# ---------------------------------------------------------------------------
echo
echo "--- 1. the observer receives S1 and stops (no ack: it has no such capability) ---"
OUT=$(CLIENT_TIMEOUT=30 client $OBS --resume from-sequence --last-acked "$((S1 - 1))" --count 1 2>&1)
echo "$OUT" | grep -q "seq=$S1" && pass "S1 ($S1) delivered" || { fail "S1 not delivered: $OUT"; }

# ---------------------------------------------------------------------------
echo
echo "--- 2. graceful restart ---"
HEAD_BEFORE=$(psql_ "SELECT last_sequence FROM altsignalgateway.sequence_state")
r "systemctl restart altsignalgateway"
sleep 6
[[ "$(r 'systemctl is-active altsignalgateway')" == active ]] \
    && pass "service came back" || fail "service did not come back"

HEAD_AFTER=$(psql_ "SELECT last_sequence FROM altsignalgateway.sequence_state")
[[ "$HEAD_AFTER" -ge "$HEAD_BEFORE" ]] \
    && pass "the sequence survived the restart ($HEAD_BEFORE -> $HEAD_AFTER)" \
    || fail "the sequence went backwards"

INFLIGHT=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal
    WHERE delivery_state IN ('QUEUED','SENT')")
[[ "$INFLIGHT" == "0" ]] \
    && pass "no signal is stranded in QUEUED/SENT after the restart" \
    || fail "$INFLIGHT signals are stranded in flight"

HASH_AFTER=$(psql_ "SELECT string_agg(payload_sha256, ',' ORDER BY sequence)
    FROM altsignalgateway.signal_journal WHERE sequence IN ($S1,$S2,$S3)")
[[ "$HASH_BEFORE" == "$HASH_AFTER" ]] \
    && pass "the payloads are byte-identical across the restart" \
    || fail "a payload hash changed across the restart"

# ---------------------------------------------------------------------------
echo
echo "--- 3. resume from S1 delivers S2 and S3, in order, once each ---"
OUT=$(CLIENT_TIMEOUT=40 client $OBS --resume from-sequence --last-acked "$S1" --count 2 2>&1)
echo "$OUT" | sed 's/^/    /'
GOT=$(echo "$OUT" | grep -oE '^seq=[0-9]+' | cut -d= -f2 | tr '\n' ' ' | sed 's/ $//')

# The invariant is NOT "exactly S2 and S3". Those were chosen minutes ago, and
# a signal's validity window is about a quarter of an hour — by now some of them
# have legitimately expired and the server is right to skip them. What must hold
# is what the protocol actually promises: strictly ascending, strictly above the
# resume point, nothing repeated.
read -r G1 G2 <<<"$GOT"
if [[ -z "${G2:-}" ]]; then
    fail "expected two signals above $S1, received [$GOT]"
else
    [[ "$G1" -gt "$S1" && "$G2" -gt "$G1" ]] \
        && pass "received [$GOT]: ascending and strictly above the resume point $S1" \
        || fail "[$GOT] is not ascending above $S1"
    [[ "$G1" != "$G2" ]] && pass "no signal was delivered twice" \
                         || fail "the same sequence arrived twice"
fi

echo "$OUT" | grep -q "^seq=$S1 " && fail "S1 was re-delivered despite being below the resume point" \
                                  || pass "S1 was not re-delivered"

# And every sequence the server skipped between S1 and G1 must have been
# skipped for a REASON — expiry — not lost.
if [[ -n "${G1:-}" && "$G1" -gt "$((S1 + 1))" ]]; then
    LIVE_SKIPPED=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal
        WHERE sequence > $S1 AND sequence < $G1
          AND expires_at_ms > (extract(epoch FROM now())*1000)::bigint
          AND delivery_state NOT IN ('EXPIRED','FAILED')")
    [[ "$LIVE_SKIPPED" == "0" ]] \
        && pass "every sequence skipped between $S1 and $G1 had expired" \
        || fail "$LIVE_SKIPPED still-valid signal(s) were skipped"
fi

# ---------------------------------------------------------------------------
echo
echo "--- 4. SIGKILL is indistinguishable from a crash ---"
PID=$(r "systemctl show altsignalgateway -p MainPID --value")
HEAD_BEFORE=$(psql_ "SELECT last_sequence FROM altsignalgateway.sequence_state")
ACKED_BEFORE=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal WHERE delivery_state='ACKED'")
r "kill -9 $PID" || true
sleep 14
[[ "$(r 'systemctl is-active altsignalgateway')" == active ]] \
    && pass "systemd restarted it after SIGKILL" || fail "it did not come back after SIGKILL"

HEAD_AFTER=$(psql_ "SELECT last_sequence FROM altsignalgateway.sequence_state")
[[ "$HEAD_AFTER" -ge "$HEAD_BEFORE" ]] \
    && pass "the sequence survived SIGKILL ($HEAD_BEFORE -> $HEAD_AFTER)" \
    || fail "the sequence went backwards after SIGKILL"

ACKED_AFTER=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal WHERE delivery_state='ACKED'")
[[ "$ACKED_AFTER" -ge "$ACKED_BEFORE" ]] \
    && pass "acknowledged signals stayed acknowledged ($ACKED_BEFORE -> $ACKED_AFTER)" \
    || fail "an acknowledgement was lost across SIGKILL"

INFLIGHT=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal
    WHERE delivery_state IN ('QUEUED','SENT')")
[[ "$INFLIGHT" == "0" ]] \
    && pass "nothing stranded in flight after SIGKILL" || fail "$INFLIGHT stranded after SIGKILL"

r "set -a; . /etc/altsignalgateway/common.env; set +a; $BIN verify \
    --config /etc/altsignalgateway/altsignalgateway.yaml --limit 10000" | tail -1 | grep -q "OK$" \
    && pass "every payload still verifies after SIGKILL" || fail "verify failed after SIGKILL"

# ---------------------------------------------------------------------------
echo
echo "--- 5. the executor's durable cursor governs redelivery ---"
CUR=$(psql_ "SELECT COALESCE(last_acked_sequence,0) FROM altsignalgateway.client_cursors
              WHERE client_id='vps2-executor'")
echo "    executor cursor = $CUR"
# Asking to go BACKWARDS must not re-deliver acknowledged trades.
# NOT --quiet: the "resume from=" line this asserts on goes to stderr and is
# suppressed by it.
OUT=$(CLIENT_TIMEOUT=25 client $EXEC --resume from-sequence --last-acked 0 --idle-exit 6 2>&1)
echo "$OUT" | sed 's/^/    /'
FROM=$(echo "$OUT" | grep -oE 'resume from=[0-9]+' | head -1 | grep -oE '[0-9]+')
[[ "${FROM:-x}" == "$CUR" ]] \
    && pass "a backwards resume request was clamped to the server's cursor ($CUR)" \
    || fail "a backwards resume was honoured: from=$FROM cursor=$CUR"

echo
[[ $rc -eq 0 ]] && echo "restart / resume: OK" || echo "restart / resume: FAILED"
exit $rc
