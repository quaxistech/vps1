#!/usr/bin/env bash
# Failure injection against the DEPLOYED service.
#
# Each case takes something away that the service depends on, or hands it
# something malformed, and asserts that the outcome is the documented one rather
# than a crash, a hang, or a silently lost trade.
#
#   usage:  ALTSGW_HOST=root@1.2.3.4 tests/shell/test_failure_injection.sh
set -uo pipefail

VPS="${ALTSGW_HOST:?set ALTSGW_HOST=user@host}"
SSH_KEY="${ALTSIGNALGATEWAY_SSH_KEY:-$HOME/.ssh/altstrategy_vps_ed25519}"
BIN=/opt/altsignalgateway/bin/altsignalgateway
CFG=/etc/altsignalgateway/altsignalgateway.yaml
CERT=/etc/altsignalgateway/tls/server.crt

rc=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; rc=1; }
note() { echo "      $1"; }

r() { ssh -o BatchMode=yes -i "$SSH_KEY" "$VPS" "$@"; }
psql_() { r "sudo -u postgres psql -d altspotter -tAc \"$1\""; }
# CLIENT_SNI is a variable, not a hardcoded flag. It was hardcoded, and the
# wrong-SNI case below appended a second --sni that the client's first-match
# argument parser ignored — so that check passed without ever testing anything.
client() {
    r "set -a; . /etc/altsignalgateway/common.env; set +a; timeout ${CLIENT_TIMEOUT:-30} $BIN client \
        --host 127.0.0.1 --port 9443 --sni ${CLIENT_SNI:-altsignalgateway} --ca-file $CERT $*"
}
OBS="--client-id vps1-observer --key-id vps1-observer-k1 --secret-env ALTSIGNALGATEWAY_CLIENT_OBSERVER_SECRET"
alive() { [[ "$(r 'systemctl is-active altsignalgateway')" == active ]]; }

echo "=== failure injection — $(date -u +%FT%TZ) ==="

BASE_ROWS=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal")
note "journal rows at start: $BASE_ROWS"

# ---------------------------------------------------------------------------
echo
echo "--- 1. malformed and hostile WebSocket input ---"
# A raw TLS client that speaks nonsense. openssl s_client gives a real TLS
# session without a WebSocket upgrade, which is the cheapest possible probe.
r "timeout 6 openssl s_client -connect 127.0.0.1:9443 -servername altsignalgateway \
     -CAfile $CERT -quiet </dev/null >/dev/null 2>&1"; true
alive && pass "survives a TLS connection that never upgrades" || fail "died on a bare TLS probe"

# HTTP where a WebSocket handshake is expected.
r "timeout 6 bash -c 'printf \"GET / HTTP/1.1\r\nHost: x\r\n\r\n\" | \
     openssl s_client -connect 127.0.0.1:9443 -servername altsignalgateway -CAfile $CERT \
     -quiet 2>/dev/null' >/dev/null"; true
alive && pass "survives a plain HTTP request" || fail "died on an HTTP request"

# Plaintext bytes at a TLS port.
r "timeout 4 bash -c 'exec 3<>/dev/tcp/127.0.0.1/9443; printf \"garbage\\n\" >&3' 2>/dev/null"; true
alive && pass "survives plaintext bytes at the TLS port" || fail "died on plaintext bytes"

# A flood of connect/disconnect cycles: the endpoint must not be exhausted by a
# flapping client.
r "for i in \$(seq 1 60); do timeout 1 bash -c 'exec 3<>/dev/tcp/127.0.0.1/9443' 2>/dev/null; done"; true
sleep 2
alive && pass "survives 60 rapid connect/disconnect cycles" || fail "died on connection flapping"
OPEN=$(psql_ "SELECT count(*) FROM altsignalgateway.sessions WHERE closed_at_ms IS NULL")
note "sessions still marked open: $OPEN"

# ---------------------------------------------------------------------------
echo
echo "--- 2. wrong credentials, wrong versions, wrong order ---"
# The reference client refuses a negative position locally, before it ever
# connects — which is what a reference client should do. The SERVER's own guard
# for the same value is still there and is covered by
# tests/unit/test_protocol.cpp's resume rules; it is simply not reachable
# through a conforming client, which is the point.
OUT=$(CLIENT_TIMEOUT=15 client $OBS --resume from-sequence --last-acked -5 --idle-exit 3 2>&1)
echo "$OUT" | grep -qE "ERR_INVALID_SEQUENCE|must be >= 0|client error" \
    && pass "a negative resume position is refused (by the client, before connecting)" \
    || fail "negative resume accepted: $OUT"

OUT=$(CLIENT_TIMEOUT=15 client --client-id vps1-observer --key-id vps2-executor-k1 \
          --secret-env ALTSIGNALGATEWAY_CLIENT_VPS2_SECRET --resume live --idle-exit 3 2>&1)
echo "$OUT" | grep -q "ERR_AUTH_FAILED" \
    && pass "a valid key used under another client's name is refused" \
    || fail "identity borrowing was accepted: $OUT"

# A certificate the client does not trust must be refused BY THE CLIENT.
OUT=$(CLIENT_TIMEOUT=15 r "set -a; . /etc/altsignalgateway/common.env; set +a; timeout 15 $BIN client \
        --host 127.0.0.1 --port 9443 --sni altsignalgateway $OBS --resume live --idle-exit 3" 2>&1)
echo "$OUT" | grep -qi "certificate verify failed" \
    && pass "the client refuses an unverifiable server certificate" \
    || fail "the client accepted an unverified certificate: $OUT"

# Wrong SNI: the certificate does not carry that name. The assertion is on the
# SPECIFIC failure, not on any error at all — an auth lockout from the preceding
# cases would otherwise make this pass without testing anything.
OUT=$(CLIENT_TIMEOUT=15 CLIENT_SNI=not-the-gateway client $OBS --resume live --idle-exit 3 2>&1)
echo "$OUT" | grep -qi "certificate verify failed" \
    && pass "hostname verification rejects a wrong SNI" \
    || fail "hostname verification did not fire: $OUT"

alive && pass "the service is unaffected by every refused connection" || fail "service died"

# ---------------------------------------------------------------------------
echo
echo "--- 3. a slow consumer that never acknowledges ---"
# --no-ack receives without acknowledging. The session must be closed after
# ack_timeout_sec, and everything it held must return to the journal.
ACKT=$(r "grep -E '^ *ack_timeout_sec' $CFG | grep -oE '[0-9]+'")
note "ack_timeout_sec = $ACKT"
NOW_MS=$(r "date +%s%3N")
LIVE=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal
    WHERE expires_at_ms > $NOW_MS AND delivery_state NOT IN ('EXPIRED','FAILED')")
if [[ "$LIVE" -gt 0 ]]; then
    CLIENT_TIMEOUT=$((ACKT + 25)) client $OBS --resume from-start --no-ack --quiet >/dev/null 2>&1 &
    BGPID=$!
    sleep $((ACKT + 15))
    wait $BGPID 2>/dev/null
    REASON=$(psql_ "SELECT close_reason FROM altsignalgateway.sessions
                     WHERE client_id='vps1-observer' ORDER BY opened_at_ms DESC LIMIT 1")
    note "session closed with: $REASON"
    [[ "$REASON" == *TIMEOUT* || "$REASON" == *QUEUE_OVERFLOW* || "$REASON" == *read_failed* ]] \
        && pass "a non-acknowledging consumer is disconnected" \
        || fail "a non-acknowledging consumer was tolerated: $REASON"

    sleep 3
    STRANDED=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal
                       WHERE delivery_state IN ('QUEUED','SENT')")
    [[ "$STRANDED" == "0" ]] \
        && pass "nothing is stranded after the slow consumer was dropped" \
        || fail "$STRANDED signals stranded in flight"
    alive && pass "the service is unaffected by the slow consumer" || fail "service died"
else
    note "(no non-expired signals; the slow-consumer case needs live inventory)"
fi

# ---------------------------------------------------------------------------
echo
echo "--- 4. a second instance is refused ---"
# It must be the WRITER LOCK that refuses it, not the port bind. A port
# collision would only stop a second instance on the same host, and it happens
# far too late — after recovery has already run and mutated delivery state.
OUT=$(r "set -a; . /etc/altsignalgateway/common.env; set +a; timeout 20 $BIN run --config $CFG" 2>&1)
echo "$OUT" | grep -qi "writer lock" \
    && pass "a second instance is refused by the journal writer lock" \
    || fail "a second instance was not refused by the lock: $(echo "$OUT" | tail -2 | tr '\n' ' ')"
echo "$OUT" | grep -q "journal_recovered" \
    && fail "the second instance ran recovery before being refused" \
    || pass "the second instance was stopped before it touched the journal"
alive && pass "the running instance is unaffected" || fail "the running instance died"

# ---------------------------------------------------------------------------
echo
echo "--- 5. NATS unavailable ---"
BEFORE=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal")
r "systemctl stop nats-server"
sleep 12
alive && pass "survives NATS being stopped" || fail "died when NATS stopped"
OUT=$(CLIENT_TIMEOUT=20 client $OBS --resume live --idle-exit 4 2>&1)
echo "$OUT" | grep -q "ready" \
    && pass "the WebSocket endpoint still serves with NATS down" \
    || fail "the endpoint stopped serving with NATS down: $OUT"
r "systemctl start nats-server"
sleep 15
alive && pass "recovers when NATS returns" || fail "did not recover"
r "journalctl -u altsignalgateway --since '-2 min' -o cat | grep -q ingest_consumer_bound" \
    && pass "the ingest consumer rebound after the outage" \
    || note "(no rebind logged yet; the reconnect backoff may not have elapsed)"
AFTER=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal")
[[ "$AFTER" -ge "$BEFORE" ]] && pass "no journal row was lost across the NATS outage ($BEFORE -> $AFTER)" \
                             || fail "the journal shrank across the outage"

# ---------------------------------------------------------------------------
echo
echo "--- 6. PostgreSQL unavailable ---"
# The service must not crash, must stop pinging the watchdog, and must recover.
r "systemctl stop postgresql"
sleep 20
STILL=$(r 'systemctl is-active altsignalgateway')
note "state with PostgreSQL down: $STILL"
r "systemctl start postgresql"
sleep 25
alive && pass "the service is running again after PostgreSQL returned" \
      || fail "the service did not recover from a PostgreSQL outage"

# The single-writer advisory lock is SESSION-scoped, so the restart above
# destroyed it. It must be re-taken, or the guarantee is gone and a second
# instance could start and run recovery. This was a real defect: pg_locks held
# nothing at all after a restart, and a second instance got as far as
# recover_in_flight().
sleep 10
LOCKS=$(psql_ "SELECT count(*) FROM pg_locks WHERE locktype='advisory'")
[[ "$LOCKS" -ge 1 ]] \
    && pass "the writer lock was re-acquired after the database restart" \
    || fail "no advisory lock is held — the single-writer guarantee is gone"
r "journalctl -u altsignalgateway --since '-3 min' -o cat | grep -q writer_lock_reacquired" \
    && pass "the re-acquisition was logged" \
    || note "(no re-acquisition log line; the lock may have survived the restart)"
r "set -a; . /etc/altsignalgateway/common.env; set +a; $BIN verify --config $CFG --limit 2000" \
    | tail -1 | grep -q "OK$" \
    && pass "the journal is intact after the PostgreSQL outage" \
    || fail "verify failed after the PostgreSQL outage"

# ---------------------------------------------------------------------------
echo
echo "--- 7. nothing was lost overall ---"
FINAL_ROWS=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal")
[[ "$FINAL_ROWS" -ge "$BASE_ROWS" ]] \
    && pass "journal rows never decreased ($BASE_ROWS -> $FINAL_ROWS)" \
    || fail "the journal lost rows ($BASE_ROWS -> $FINAL_ROWS)"
# Violations are a permanent audit record (1000-day retention), so the
# adversarial-input suite deliberately leaves some behind. What must hold here is
# that THIS run produced none.
VIOLATIONS=$(psql_ "SELECT count(*) FROM altsignalgateway.integrity_violations
                     WHERE created_at > now() - interval '20 min'")
TOTAL_VIOLATIONS=$(psql_ "SELECT count(*) FROM altsignalgateway.integrity_violations")
note "integrity violations: $VIOLATIONS in this run, $TOTAL_VIOLATIONS on record"
[[ "$VIOLATIONS" == "0" ]] && pass "no integrity violation was caused by these failures" \
                           || fail "$VIOLATIONS integrity violation(s) during this run"
FAILED=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal WHERE delivery_state='FAILED'")
[[ "$FAILED" == "0" ]] && pass "no signal is in FAILED" || fail "$FAILED signals are FAILED"

echo
[[ $rc -eq 0 ]] && echo "failure injection: OK" || echo "failure injection: FAILED"
exit $rc
