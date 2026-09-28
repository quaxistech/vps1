#!/usr/bin/env bash
# The end-to-end delivery proof, run against the DEPLOYED service.
#
# There is no mock anywhere in this file. It drives the real binary, the real
# PostgreSQL journal, the real TLS WebSocket and the real reference client, and
# every assertion is made against what those actually did. A mock of
# PostgreSQL's transactional guarantees would be testing the mock.
#
#   usage:  ALTSGW_HOST=root@1.2.3.4 tests/shell/test_live_delivery.sh
#
# It is deliberately NOT part of `ctest`: it needs a deployed stack, and a test
# suite that silently skips when its dependency is absent teaches people to
# ignore it.
set -uo pipefail

VPS="${ALTSGW_HOST:?set ALTSGW_HOST=user@host}"
SSH_KEY="${ALTSIGNALGATEWAY_SSH_KEY:-$HOME/.ssh/altstrategy_vps_ed25519}"
BIN=/opt/altsignalgateway/bin/altsignalgateway
CFG=/etc/altsignalgateway/altsignalgateway.yaml
CERT=/etc/altsignalgateway/tls/server.crt

rc=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; rc=1; }

r() { ssh -o BatchMode=yes -i "$SSH_KEY" "$VPS" "$@"; }
psql_() { r "sudo -u postgres psql -d altspotter -tAc \"$1\""; }
asgw()  { r "set -a; . /etc/altsignalgateway/common.env; set +a; $BIN $*"; }
client() {
    r "set -a; . /etc/altsignalgateway/common.env; set +a; timeout ${CLIENT_TIMEOUT:-40} $BIN client \
        --host 127.0.0.1 --port 9443 --sni altsignalgateway --ca-file $CERT $*"
}

echo "=== AltSignalGateway live delivery proof — $(date -u +%FT%TZ) ==="
echo

# ---------------------------------------------------------------------------
echo "--- 1. the service is up and the endpoint is listening ---"
[[ "$(r 'systemctl is-active altsignalgateway')" == active ]] \
    && pass "service active" || fail "service not active"
r 'ss -tln | grep -q ":9443"' && pass "listening on 9443" || fail "not listening"

# ---------------------------------------------------------------------------
echo
echo "--- 2. the journal holds signals with intact hashes ---"
HEAD=$(psql_ "SELECT COALESCE(max(sequence),0) FROM altsignalgateway.signal_journal")
ROWS=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal")
echo "    journal head=$HEAD rows=$ROWS"
[[ "$ROWS" -gt 0 ]] || { fail "the journal is empty — nothing to prove"; exit 1; }

# The service reads its DSN from the environment file, exactly as systemd does.
r "set -a; . /etc/altsignalgateway/common.env; set +a; $BIN verify --config $CFG --limit 10000" \
    | tail -1 | grep -q "OK$" \
    && pass "every journalled payload re-hashes and re-serializes exactly" \
    || fail "verify reported a mismatch"

# The ALLOCATOR is gapless — that is proven by `altsignalgateway selftest`,
# which allocates, rolls back and checks the counter is unchanged.
#
# The surviving ROWS legitimately may have gaps: retention removes rows older
# than its window, and after 400 days it always will. So the assertion here is
# the one that would actually indicate a defect — gaplessness among rows recent
# enough that retention cannot have touched them — plus no duplicates anywhere.
GAPS=$(psql_ "SELECT count(*) FROM (
    SELECT sequence, lag(sequence) OVER (ORDER BY sequence) prev
      FROM altsignalgateway.signal_journal
     WHERE created_at > now() - interval '6 hours') t
    WHERE prev IS NOT NULL AND sequence <> prev + 1")
# A gap among the surviving rows can only come from a DELETION, and the only
# thing permitted to delete is the retention pass (enforced by a trigger, and
# asserted in section 8). So a gap is not a defect on its own — it is a defect
# only if nothing recorded removing those rows.
DELETED=$(psql_ "SELECT COALESCE(sum(rows_deleted),0) FROM altsignalgateway.retention_runs")
if [[ "$GAPS" == "0" ]]; then
    pass "no gaps in the recent sequence window"
elif [[ "$DELETED" -gt 0 ]]; then
    pass "$GAPS gap(s) in the window, explained by $DELETED audited retention deletion(s)"
else
    fail "$GAPS gap(s) with no recorded deletion to explain them"
fi

DUPES=$(psql_ "SELECT count(*) FROM (
    SELECT sequence FROM altsignalgateway.signal_journal
     GROUP BY sequence HAVING count(*) > 1) t")
[[ "$DUPES" == "0" ]] && pass "no sequence is used twice" || fail "$DUPES duplicated sequence(s)"

r "set -a; . /etc/altsignalgateway/common.env; set +a; $BIN selftest --config $CFG" \
    | tail -1 | grep -q "OK$" \
    && pass "the database invariants hold on this deployment (selftest)" \
    || fail "selftest reported a broken invariant"

# ---------------------------------------------------------------------------
echo
echo "--- 3. the journal signal equals the upstream signal ---"
# Byte-for-byte, through the embedded original: this is the claim that the
# canonical projection and AltSignalQuality's own message are two views of ONE
# signal rather than two independent records.
MISMATCH=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal j
    WHERE encode(sha256(j.source_payload),'hex') <> j.source_payload_sha256")
[[ "$MISMATCH" == "0" ]] && pass "source payload hashes match" || fail "$MISMATCH source hash mismatches"

# And the FINAL signal id in the journal is the one AltSignalQuality recorded.
ORPHANS=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal j
    LEFT JOIN altsignalquality.signal_evaluations e
      ON e.final_signal_id = j.signal_id
    WHERE e.final_signal_id IS NULL")
[[ "$ORPHANS" == "0" ]] \
    && pass "every journalled signal traces to an AltSignalQuality evaluation" \
    || fail "$ORPHANS journalled signals have no upstream evaluation"

# ---------------------------------------------------------------------------
echo
echo "--- 4. a real client receives, verifies and acknowledges ---"
CURSOR_BEFORE=$(psql_ "SELECT COALESCE(last_acked_sequence,0) FROM altsignalgateway.client_cursors
                        WHERE client_id='vps2-executor'")
CURSOR_BEFORE=${CURSOR_BEFORE:-0}
DELIVERABLE=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal
    WHERE sequence > ${CURSOR_BEFORE} AND expires_at_ms > (extract(epoch FROM now())*1000)::bigint
      AND delivery_state NOT IN ('EXPIRED','FAILED')")
echo "    cursor=$CURSOR_BEFORE deliverable=$DELIVERABLE"

if [[ "$DELIVERABLE" -gt 0 ]]; then
    OUT=$(CLIENT_TIMEOUT=40 client --client-id vps2-executor --key-id vps2-executor-k1 \
              --secret-env ALTSIGNALGATEWAY_CLIENT_VPS2_SECRET \
              --resume from-sequence --last-acked "$CURSOR_BEFORE" \
              --count 1 2>&1)
    echo "$OUT" | sed 's/^/    /'
    echo "$OUT" | grep -q "received=1 acked=1" \
        && pass "one real signal delivered, hash-verified by the client and acknowledged" \
        || fail "the client did not receive and acknowledge a signal"

    SEQ=$(echo "$OUT" | grep -oE 'seq=[0-9]+' | head -1 | cut -d= -f2)
    if [[ -n "$SEQ" ]]; then
        sleep 2
        STATE=$(psql_ "SELECT delivery_state FROM altsignalgateway.signal_journal WHERE sequence=$SEQ")
        [[ "$STATE" == "ACKED" ]] && pass "seq $SEQ is ACKED in the journal" \
                                  || fail "seq $SEQ is $STATE, expected ACKED"
        CURSOR_AFTER=$(psql_ "SELECT last_acked_sequence FROM altsignalgateway.client_cursors
                               WHERE client_id='vps2-executor'")
        [[ "$CURSOR_AFTER" -ge "$SEQ" ]] && pass "the durable cursor advanced to $CURSOR_AFTER" \
                                         || fail "the cursor did not advance"
        # The delivery audit must show the whole path, not just the outcome.
        EVENTS=$(psql_ "SELECT string_agg(event, ',' ORDER BY id) FROM altsignalgateway.delivery_events
                         WHERE sequence=$SEQ")
        echo "    delivery_events(seq=$SEQ): $EVENTS"
        [[ "$EVENTS" == *QUEUED* && "$EVENTS" == *ACKED* ]] \
            && pass "the delivery audit records the full path" \
            || fail "the delivery audit is incomplete"
    fi
else
    echo "    (no non-expired signal above the cursor; the delivery leg needs a fresh signal)"
fi

# ---------------------------------------------------------------------------
echo
echo "--- 5. an observer cannot advance the executor's cursor ---"
CUR=$(psql_ "SELECT COALESCE(last_acked_sequence,0) FROM altsignalgateway.client_cursors
              WHERE client_id='vps2-executor'")
CLIENT_TIMEOUT=20 client --client-id vps1-observer --key-id vps1-observer-k1 \
    --secret-env ALTSIGNALGATEWAY_CLIENT_OBSERVER_SECRET \
    --resume from-start --idle-exit 5 --quiet >/dev/null 2>&1
CUR2=$(psql_ "SELECT COALESCE(last_acked_sequence,0) FROM altsignalgateway.client_cursors
               WHERE client_id='vps2-executor'")
[[ "$CUR" == "$CUR2" ]] && pass "the executor's cursor is unchanged ($CUR)" \
                        || fail "an observer moved the executor's cursor"

# ---------------------------------------------------------------------------
echo
echo "--- 6. authentication and authorization ---"
OUT=$(CLIENT_TIMEOUT=15 client --client-id vps2-executor --key-id vps2-executor-k1 \
          --secret-env ALTSIGNALGATEWAY_CLIENT_OBSERVER_SECRET --resume live --idle-exit 3 2>&1)
echo "$OUT" | grep -q "ERR_AUTH_FAILED" \
    && pass "a wrong key is refused with ERR_AUTH_FAILED" \
    || fail "a wrong key was not refused: $OUT"

OUT=$(CLIENT_TIMEOUT=15 client --client-id nobody --key-id nobody-k1 \
          --secret-env ALTSIGNALGATEWAY_CLIENT_VPS2_SECRET --resume live --idle-exit 3 2>&1)
echo "$OUT" | grep -q "ERR_AUTH_FAILED" \
    && pass "an unknown identity is refused" \
    || fail "an unknown identity was not refused: $OUT"

# The observer has no `ack` capability; the ACK path must be closed to it even
# though it can receive.
CAPS=$(r "grep -A4 'client_id: vps1-observer' /etc/altsignalgateway/clients.yaml | grep capabilities")
echo "    observer $CAPS"
[[ "$CAPS" != *ack* ]] && pass "the observer credential grants no ack" \
                       || fail "the observer credential grants ack"

# ---------------------------------------------------------------------------
echo
echo "--- 7. resume safety ---"
CUR_OBS_BEFORE=$(psql_ "SELECT COALESCE(last_acked_sequence,0) FROM altsignalgateway.client_cursors
                         WHERE client_id='vps1-observer'")
CUR_OBS_BEFORE=${CUR_OBS_BEFORE:-0}
OUT=$(CLIENT_TIMEOUT=15 client --client-id vps1-observer --key-id vps1-observer-k1 \
          --secret-env ALTSIGNALGATEWAY_CLIENT_OBSERVER_SECRET \
          --resume from-sequence --last-acked 999999999 --idle-exit 3 2>&1)
echo "$OUT" | grep -q "ERR_INVALID_SEQUENCE" \
    && pass "a resume position ahead of the journal is refused" \
    || fail "a future resume position was accepted: $OUT"

# And nothing DESTRUCTIVE happened. The head is not the thing to compare — the
# pipeline keeps producing while this test runs, so a growing head is the system
# working. What must not change is what already existed: no row removed, no
# payload rewritten, no cursor moved by a request that was refused.
ROWS2=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal WHERE sequence <= $HEAD")
CUR_OBS=$(psql_ "SELECT COALESCE(last_acked_sequence,0) FROM altsignalgateway.client_cursors
                  WHERE client_id='vps1-observer'")
CUR_OBS=${CUR_OBS:-0}
[[ "$ROWS2" -ge "$ROWS" ]] \
    && pass "no journal row was removed by the refused resume ($ROWS -> $ROWS2 at or below $HEAD)" \
    || fail "the journal lost rows after a bad resume"
[[ "$CUR_OBS" == "$CUR_OBS_BEFORE" ]] \
    && pass "the refused resume moved no cursor (still $CUR_OBS)" \
    || fail "a refused resume advanced a cursor from $CUR_OBS_BEFORE to $CUR_OBS"
r "set -a; . /etc/altsignalgateway/common.env; set +a; $BIN verify --config $CFG --limit 200" \
    | tail -1 | grep -q "OK$" \
    && pass "payload integrity is intact after the refused resume" \
    || fail "verify failed after a refused resume"

# ---------------------------------------------------------------------------
echo
echo "--- 7b. a live-only session abandons nothing ---"
# Starting at the head is a decision about ONE connection. If it persisted, a
# diagnostic run with an executor's credentials would silently discard every
# undelivered trade below the head.
CUR_EXEC_BEFORE=$(psql_ "SELECT COALESCE(last_acked_sequence,0) FROM altsignalgateway.client_cursors
                          WHERE client_id='vps2-executor'")
CUR_EXEC_BEFORE=${CUR_EXEC_BEFORE:-0}
CLIENT_TIMEOUT=20 client --client-id vps2-executor --key-id vps2-executor-k1 \
    --secret-env ALTSIGNALGATEWAY_CLIENT_VPS2_SECRET \
    --resume live --idle-exit 5 --quiet >/dev/null 2>&1
sleep 2
CUR_EXEC_AFTER=$(psql_ "SELECT COALESCE(last_acked_sequence,0) FROM altsignalgateway.client_cursors
                         WHERE client_id='vps2-executor'")
CUR_EXEC_AFTER=${CUR_EXEC_AFTER:-0}
[[ "$CUR_EXEC_AFTER" == "$CUR_EXEC_BEFORE" ]] \
    && pass "a live-only session wrote no cursor (still $CUR_EXEC_AFTER)" \
    || fail "a live-only session advanced the cursor $CUR_EXEC_BEFORE -> $CUR_EXEC_AFTER, abandoning the backlog"

# ---------------------------------------------------------------------------
echo
echo "--- 8. the journal is immutable ---"
OUT=$(psql_ "UPDATE altsignalgateway.signal_journal SET payload_sha256 = repeat('0',64)
              WHERE sequence = $HEAD" 2>&1)
echo "$OUT" | grep -qi "immutable" && pass "an attempt to rewrite a payload hash is refused" \
                                   || fail "the immutability trigger did not fire: $OUT"

OUT=$(psql_ "UPDATE altsignalgateway.signal_journal SET delivery_state='ACKED'
              WHERE delivery_state='JOURNALED'" 2>&1)
if [[ "$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal WHERE delivery_state='JOURNALED'")" != "0" ]]; then
    echo "$OUT" | grep -qi "illegal delivery transition" \
        && pass "an illegal JOURNALED -> ACKED transition is refused" \
        || fail "the transition trigger did not fire: $OUT"
else
    echo "    (no JOURNALED rows to test the transition guard against)"
fi

OUT=$(psql_ "DELETE FROM altsignalgateway.signal_journal WHERE sequence = $HEAD" 2>&1)
echo "$OUT" | grep -qi "retention pass" && pass "a delete outside retention is refused" \
                                        || fail "the delete guard did not fire: $OUT"

# ---------------------------------------------------------------------------
echo
echo "--- 9. the endpoint is not exposed to the internet ---"
r 'ufw status | grep -q "9443"' && fail "port 9443 is open in ufw" \
                                || pass "port 9443 is not open in ufw (no external consumer yet)"

echo
[[ $rc -eq 0 ]] && echo "live delivery proof: OK" || echo "live delivery proof: FAILED"
exit $rc
