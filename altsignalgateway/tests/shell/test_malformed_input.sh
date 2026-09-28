#!/usr/bin/env bash
# Adversarial inbound messages, published to the REAL subject by the REAL
# producer account, and consumed by the REAL deployed service.
#
# Every case below asserts three things:
#   * the service survives it;
#   * it is recorded, so a contract break is diagnosable from the database
#     rather than from a log that has rotated away;
#   * it never becomes a journalled signal that a consumer could act on.
#
# The publisher is a separate binary (asgw-inject) built from tests/tools and
# carried to the host for this run: AltSignalGateway's own NATS account has no
# publish permission on `signalquality.>` and must never have one.
#
#   usage:  ALTSGW_HOST=root@1.2.3.4 tests/shell/test_malformed_input.sh
set -uo pipefail

VPS="${ALTSGW_HOST:?set ALTSGW_HOST=user@host}"
SSH_KEY="${ALTSIGNALGATEWAY_SSH_KEY:-$HOME/.ssh/altstrategy_vps_ed25519}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INJECT_LOCAL="$REPO_ROOT/build/release/bin/asgw-inject"
INJECT_REMOTE=/tmp/asgw-inject
BIN=/opt/altsignalgateway/bin/altsignalgateway
CFG=/etc/altsignalgateway/altsignalgateway.yaml
SUBJECT=signalquality.final.probe.v1

rc=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; rc=1; }
note() { echo "      $1"; }

r() { ssh -o BatchMode=yes -i "$SSH_KEY" "$VPS" "$@"; }
psql_() { r "sudo -u postgres psql -d altspotter -tAc \"$1\""; }
alive() { [[ "$(r 'systemctl is-active altsignalgateway')" == active ]]; }

[[ -x "$INJECT_LOCAL" ]] || { echo "build asgw-inject first (cmake --build --preset release)"; exit 2; }

echo "=== malformed input — $(date -u +%FT%TZ) ==="

# The producer's credentials, read on the host so they never cross the wire.
NATS_URL=$(r "grep -oP '(?<=^ALTSIGNALQUALITY_NATS_URL=).*' /etc/altsignalquality/common.env")
[[ -n "$NATS_URL" ]] || { echo "could not read the producer's NATS url"; exit 2; }

scp -q -o BatchMode=yes -i "$SSH_KEY" "$INJECT_LOCAL" "$VPS:$INJECT_REMOTE"
r "chmod +x $INJECT_REMOTE"
cleanup() { r "rm -f $INJECT_REMOTE /tmp/asgw-source.bin" >/dev/null 2>&1; }
trap cleanup EXIT

BEFORE_ROWS=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal")
BEFORE_REJECTED=$(psql_ "SELECT count(*) FROM altsignalgateway.rejected_messages")
BEFORE_VIOLATIONS=$(psql_ "SELECT count(*) FROM altsignalgateway.integrity_violations")
note "journal=$BEFORE_ROWS rejected=$BEFORE_REJECTED violations=$BEFORE_VIOLATIONS"

inject() { r "$INJECT_REMOTE --url '$NATS_URL' --subject $SUBJECT --case $1 ${2:-}"; }

# ---------------------------------------------------------------------------
echo
echo "--- 1. messages that are not decodable at all ---"
for c in garbage truncated empty; do
    OUT=$(inject "$c" 2>&1) || { fail "could not publish case $c: $OUT"; continue; }
    note "$OUT"
done
sleep 6
alive && pass "the service survived undecodable input" || fail "the service died"

# ---------------------------------------------------------------------------
echo
echo "--- 2. decodable messages that break the contract ---"
for c in wrong-service bad-schema rejected-decision bad-geometry nan-price no-expiry; do
    OUT=$(inject "$c" 2>&1) || { fail "could not publish case $c: $OUT"; continue; }
    note "$OUT"
done
sleep 8
alive && pass "the service survived contract-breaking input" || fail "the service died"

# ---------------------------------------------------------------------------
echo
echo "--- 3. a structurally valid message that is too large to transmit ---"
# Every free-text field on an upstream proposal is unbounded, so this is a
# message the producer's own contract permits. It must be refused here rather
# than journalled: a payload larger than the bound this endpoint advertises in
# AUTH_OK is a delivery it could never make.
OUT=$(inject oversized 2>&1) && note "$OUT" || note "publish refused by the broker: $OUT"
sleep 6
alive && pass "the service survived a ~900 KB message" || fail "the service died"
TOO_LARGE=$(psql_ "SELECT count(*) FROM altsignalgateway.rejected_messages
                    WHERE rejection_code='PAYLOAD_TOO_LARGE' AND created_at > now() - interval '5 min'")
[[ "$TOO_LARGE" -ge 1 ]] \
    && pass "it was refused as PAYLOAD_TOO_LARGE, not journalled" \
    || fail "an untransmittable payload was not refused"

# ---------------------------------------------------------------------------
echo
echo "--- 4. a payload that is not a final signal at all ---"
OUT=$(inject not-final 2>&1) && note "$OUT"
sleep 5
alive && pass "the service survived a non-final payload" || fail "the service died"

# ---------------------------------------------------------------------------
echo
echo "--- what was recorded ---"
AFTER_ROWS=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal")
AFTER_REJECTED=$(psql_ "SELECT count(*) FROM altsignalgateway.rejected_messages")
note "journal=$AFTER_ROWS (was $BEFORE_ROWS)   rejected=$AFTER_REJECTED (was $BEFORE_REJECTED)"
r "sudo -u postgres psql -d altspotter -c \"
   SELECT rejection_code, count(*), max(payload_bytes) AS max_bytes
     FROM altsignalgateway.rejected_messages
    WHERE created_at > now() - interval '5 min' GROUP BY 1 ORDER BY 2 DESC;\""

[[ "$AFTER_REJECTED" -gt "$BEFORE_REJECTED" ]] \
    && pass "every refusal was recorded in rejected_messages" \
    || fail "nothing was recorded — a contract break would be invisible"

# The decisive assertion: not one of those became a deliverable signal.
# (An earlier build journalled the oversized case; that row is removed by
#  tests/shell/cleanup_inject.sh, which is the documented retention escape.)
INJECTED=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal
                   WHERE signal_id LIKE 'sqfin_inject_%'")
[[ "$INJECTED" == "0" ]] \
    && pass "no adversarial message became a journalled signal" \
    || fail "$INJECTED adversarial messages were journalled"

# A message the service refuses must be TERMINATED, not redelivered forever.
REDELIVERED=$(r "journalctl -u altsignalgateway --since '-5 min' -o cat | grep -c signal_rejected")
note "signal_rejected log records in the window: $REDELIVERED"
sleep 10
REDELIVERED2=$(r "journalctl -u altsignalgateway --since '-5 min' -o cat | grep -c signal_rejected")
[[ "$REDELIVERED2" == "$REDELIVERED" ]] \
    && pass "refused messages are terminated, not redelivered in a loop" \
    || fail "the count grew from $REDELIVERED to $REDELIVERED2 — the consumer is wedged"

# ---------------------------------------------------------------------------
echo
echo "--- 5. the same signal id, republished unchanged (idempotency) ---"
# A real signal from the pipeline, never one this test injected.
SEQ=$(psql_ "SELECT max(sequence) FROM altsignalgateway.signal_journal
              WHERE signal_id NOT LIKE 'sqfin_inject_%'")
if [[ -z "$SEQ" || "$SEQ" == "0" ]]; then
    note "(no journalled signal to replay)"
else
    SIGNAL_ID=$(psql_ "SELECT signal_id FROM altsignalgateway.signal_journal WHERE sequence=$SEQ")
    SUBJ=$(psql_ "SELECT source_subject FROM altsignalgateway.signal_journal WHERE sequence=$SEQ")
    HASH_BEFORE=$(psql_ "SELECT payload_sha256 FROM altsignalgateway.signal_journal WHERE sequence=$SEQ")
    # The exact bytes AltSignalQuality published, taken back out of the journal.
    r "sudo -u postgres psql -d altspotter -tAc \"SELECT encode(source_payload,'base64') FROM altsignalgateway.signal_journal WHERE sequence=$SEQ\" | tr -d '\n' | base64 -d > /tmp/asgw-source.bin"
    note "replaying $SIGNAL_ID (seq $SEQ) on $SUBJ"

    OUT=$(r "$INJECT_REMOTE --url '$NATS_URL' --subject '$SUBJ' --case duplicate --source-file /tmp/asgw-source.bin" 2>&1)
    note "$OUT"
    sleep 6
    ROWS=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal WHERE signal_id='$SIGNAL_ID'")
    [[ "$ROWS" == "1" ]] && pass "an identical republish produced no second row" \
                         || fail "$ROWS rows for one signal id"
    HASH_AFTER=$(psql_ "SELECT payload_sha256 FROM altsignalgateway.signal_journal WHERE sequence=$SEQ")
    [[ "$HASH_BEFORE" == "$HASH_AFTER" ]] && pass "the journalled payload is unchanged" \
                                          || fail "the payload changed under a republish"
    r "journalctl -u altsignalgateway --since '-2 min' -o cat | grep -q signal_already_journalled" \
        && pass "the redelivery was recognised as a duplicate" \
        || note "(no duplicate log line; JetStream may have de-duplicated it first)"

    # ------------------------------------------------------------------
    echo
    echo "--- 6. the same signal id with a DIFFERENT payload (integrity) ---"
    OUT=$(r "$INJECT_REMOTE --url '$NATS_URL' --subject '$SUBJ' --case conflict --source-file /tmp/asgw-source.bin" 2>&1)
    note "$OUT"
    sleep 6
    AFTER_VIOLATIONS=$(psql_ "SELECT count(*) FROM altsignalgateway.integrity_violations")
    [[ "$AFTER_VIOLATIONS" -gt "$BEFORE_VIOLATIONS" ]] \
        && pass "the conflict was recorded as an integrity violation" \
        || fail "a conflicting payload was accepted silently"
    HASH_FINAL=$(psql_ "SELECT payload_sha256 FROM altsignalgateway.signal_journal WHERE sequence=$SEQ")
    [[ "$HASH_BEFORE" == "$HASH_FINAL" ]] \
        && pass "the ORIGINAL payload was kept, not overwritten" \
        || fail "the journalled payload was replaced by the conflicting one"
    ROWS=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal WHERE signal_id='$SIGNAL_ID'")
    [[ "$ROWS" == "1" ]] && pass "no second row was created for the conflict" \
                         || fail "$ROWS rows for one signal id"
    r "journalctl -u altsignalgateway --since '-2 min' -o cat | grep -q signal_integrity_violation" \
        && pass "it was logged at critical" || fail "the violation was not logged"
    r "sudo -u postgres psql -d altspotter -c \"
       SELECT signal_id, left(existing_sha256,16) AS existing, left(incoming_sha256,16) AS incoming
         FROM altsignalgateway.integrity_violations ORDER BY id DESC LIMIT 3;\""
fi

# ---------------------------------------------------------------------------
echo
echo "--- the journal is still sound ---"
r "set -a; . /etc/altsignalgateway/common.env; set +a; $BIN verify --config $CFG --limit 10000" \
    | tail -1 | grep -q "OK$" && pass "every payload still verifies" || fail "verify failed"
alive && pass "the service is still running" || fail "the service is not running"

echo
[[ $rc -eq 0 ]] && echo "malformed input: OK" || echo "malformed input: FAILED"
exit $rc
