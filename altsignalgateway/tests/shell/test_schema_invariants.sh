#!/usr/bin/env bash
# The database half of the invariants.
#
# The delivery state machine and the immutability rules exist TWICE: in
# domain/delivery_state.hpp so the process never attempts an illegal write, and
# in the migration so an illegal history is impossible regardless of what wrote
# it. Two copies of a rule are two copies that can drift, so this test asserts
# the SQL says what the C++ says.
#
# Static analysis of the migration text — it needs no database, so it runs in
# every build rather than only where PostgreSQL happens to be reachable.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SQL="$REPO_ROOT/db/migrations/0001_altsignalgateway_schema.sql"
HPP="$REPO_ROOT/src/altsignalgateway/domain/delivery_state.hpp"
rc=0

fail() { echo "FAIL  $1"; rc=1; }
ok()   { echo "OK    $1"; }

[[ -f "$SQL" ]] || { echo "FAIL  migration not found"; exit 1; }

# --- the state vocabulary must match ----------------------------------------
before_vocab=$rc
for state in JOURNALED QUEUED SENT ACKED EXPIRED FAILED; do
    grep -q "'$state'" "$SQL" || fail "the migration does not know state $state"
    grep -q "\"$state\"" "$HPP" || fail "delivery_state.hpp does not know state $state"
done
grep -q "CHECK (delivery_state IN ('JOURNALED','QUEUED','SENT','ACKED','EXPIRED','FAILED'))" "$SQL" \
    || fail "delivery_state is not constrained to the six known states"
[[ $rc -eq $before_vocab ]] && ok "the delivery state vocabulary matches on both sides"

# --- the transition table must match -----------------------------------------
check_transition() {
    local from="$1" to="$2"
    # The SQL aligns the WHEN arms with padding, so the pattern is
    # whitespace-tolerant.
    grep -A2 -E "WHEN '$from' +THEN NEW.delivery_state IN" "$SQL" | grep -q "'$to'" \
        || fail "the trigger does not allow $from -> $to"
}
before_transitions=$rc
check_transition JOURNALED QUEUED
check_transition JOURNALED EXPIRED
check_transition JOURNALED FAILED
check_transition QUEUED SENT
check_transition QUEUED JOURNALED
check_transition SENT ACKED
check_transition SENT JOURNALED
grep -q "ELSE false" "$SQL" || fail "terminal states are not closed in the trigger"
[[ $rc -eq $before_transitions ]] && ok "the transition table matches domain/delivery_state.hpp"

# --- immutability -------------------------------------------------------------
for column in signal_id sequence created_at_ms expires_at_ms source_payload \
              source_payload_sha256 canonical_payload payload_sha256 schema_version; do
    grep -q "NEW.$column IS DISTINCT FROM OLD.$column" "$SQL" \
        || fail "the immutability trigger does not protect $column"
done
ok "the immutability trigger protects signal identity and both payloads"

# --- the sequence -------------------------------------------------------------
# Comment lines are stripped first: the migration EXPLAINS why a BIGSERIAL was
# rejected, and matching that prose would be matching the wrong thing.
if grep -v '^ *--' "$SQL" | grep -qi "bigserial"; then
    fail "the journal must not use a BIGSERIAL sequence (gaps + out-of-order visibility)"
fi
grep -q "sequence_state" "$SQL" || fail "the persistent sequence counter is missing"
grep -q "sequence *BIGINT NOT NULL UNIQUE" "$SQL" || fail "the journal sequence is not unique"
grep -q "chk_journal_sequence_positive" "$SQL" || fail "the sequence is not constrained positive"
ok "the sequence is a row-locked counter, unique and positive"

# --- other invariants ---------------------------------------------------------
grep -q "signal_id            TEXT PRIMARY KEY" "$SQL" || fail "signal_id is not the primary key"
grep -q "message_id           TEXT NOT NULL UNIQUE" "$SQL" || fail "message_id is not unique"
grep -q "chk_journal_hash_shape" "$SQL" || fail "hash columns are not shape-checked"
grep -q "chk_journal_acked_evidence" "$SQL" || fail "ACKED rows may be written without evidence"
grep -q "chk_journal_sent_evidence" "$SQL" || fail "SENT rows may be written without evidence"
grep -q "enforce_cursor_monotonic" "$SQL" || fail "a client cursor may move backwards"
grep -q "guard_journal_delete" "$SQL" || fail "journal rows may be deleted outside retention"
ok "identity, hash, evidence, cursor and deletion invariants are present"

# --- the service must never be given more than it needs ------------------------
BOOTSTRAP="$REPO_ROOT/scripts/db-bootstrap.sh"
grep -q "GRANT SELECT ON" "$BOOTSTRAP" || fail "no read-only grants in db-bootstrap.sh"
if grep -E "GRANT (INSERT|UPDATE|DELETE|ALL)" "$BOOTSTRAP" | grep -qE "altstrategy|public\."; then
    fail "db-bootstrap.sh grants write access to another service's schema"
fi
ok "the service role has SELECT only on the upstream schemas"

[[ $rc -eq 0 ]] && echo "schema invariants: OK" || echo "schema invariants: FAILED"
exit $rc
