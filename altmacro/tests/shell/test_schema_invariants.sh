#!/usr/bin/env bash
# Schema invariants that keep silent corruption impossible.
#
# Each of these is a property the database enforces so that the code does not
# have to remember it. They are asserted here because a future migration could
# relax one without any test failing anywhere else.
#
#   sudo -u postgres ./test_schema_invariants.sh [dbname]
set -euo pipefail
DB="${1:-altmacro}"
fails=0

check() {
    local name="$1" sql="$2" expect="$3"
    local got
    got="$(psql -X -q -t -A -d "$DB" -c "$sql" | tr -d '[:space:]')"
    if [[ "$got" == "$expect" ]]; then
        echo "  ok    $name"
    else
        echo "  FAIL  $name (expected '$expect', got '$got')"
        fails=$((fails + 1))
    fi
}

echo "schema invariants on $DB"

# The defect that was actually found: bid and ask are fetched from separate
# files and are separate series. If they shared a cursor, the first side to
# finish would mark the window complete and the second would be skipped in
# silence -- half the data lost with nothing in any log.
check "cursor key includes the price basis" "
    SELECT count(*) FROM pg_index i
    JOIN pg_class c ON c.oid = i.indrelid
    JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum = ANY(i.indkey)
    WHERE c.relname='macro_ingest_cursors' AND i.indisprimary AND a.attname='price_basis'" "1"

# The core anti-zero rule. A yield series that stored an absence as 0.0 would
# describe a market event that never happened, and would do so plausibly.
#
# macro_price_bars is partitioned and every partition inherits these, so the
# question is whether the constraint exists at all, not how many copies of it
# the catalogue holds.
check "a non-valid bar cannot carry a price" "
    SELECT (count(*) > 0)::int FROM pg_constraint
    WHERE conname='chk_bar_absence_is_explicit'" "1"
check "a valid bar must carry all four prices" "
    SELECT (count(*) > 0)::int FROM pg_constraint
    WHERE conname='chk_bar_valid_has_value'" "1"
check "a non-valid reference row cannot carry a value" "
    SELECT (count(*) > 0)::int FROM pg_constraint
    WHERE conname='chk_ref_absence_is_explicit'" "1"

# Revisions must coexist rather than overwrite: without the vintage in the key,
# history would silently rewrite itself every time a statistic was revised.
check "a release is keyed by its vintage" "
    SELECT count(*) FROM pg_constraint
    WHERE conrelid='macro_events'::regclass AND contype='u'
      AND pg_get_constraintdef(oid) LIKE '%vintage_date%'" "1"

# A decode scale without a band is a ten-times error waiting to happen.
check "a decode scale requires a sanity band" "
    SELECT (count(*) > 0)::int FROM pg_constraint
    WHERE conname='chk_scale_has_sanity_band'" "1"

# Fallback order is resolved within one basis, so a provider switch cannot move
# a series from bid to mid without anyone noticing.
check "fallback priority is unique per basis" "
    SELECT count(*) FROM pg_constraint
    WHERE conrelid='macro_instrument_sources'::regclass AND contype='u'
      AND pg_get_constraintdef(oid) LIKE '%price_basis%priority%'" "1"

# The three CNY/CNH semantics must exist as three separate instruments.
check "CNY/CNH are three instruments, not one" "
    SELECT count(*) FROM macro_instruments
    WHERE code IN ('FX.USDCNH','REF.USDCNY.FED','REF.USDCNY.PBOC')" "3"
check "the offshore quote is MARKET and the references are not" "
    SELECT count(*) FROM macro_instruments
    WHERE code='FX.USDCNH' AND series_class='MARKET'" "1"

# A documented limit must name the document it was read from; an undocumented
# one must not pretend to.
check "every documented rate limit cites a source" "
    SELECT count(*) FROM macro_providers
    WHERE rate_limit_source='documented' AND rate_limit_ref IS NULL" "0"

# No stored row may ever be a zero standing in for an absence.
check "no bar carries a zero price" "
    SELECT count(*) FROM macro_price_bars
    WHERE open = 0 OR high = 0 OR low = 0 OR close = 0" "0"
check "no absent bar carries a price" "
    SELECT count(*) FROM macro_price_bars
    WHERE quality_status NOT IN ('VALID','STALE','PARTIAL') AND close IS NOT NULL" "0"

# The poller re-reads a window the archive may still be writing, so an early
# read can legitimately return a carried-forward price where ticks later appear.
# That direction is an upgrade. The reverse would quietly erase real ticks on
# every poll, so a STALE row that carries volume is evidence it happened.
check "no carried-forward row claims volume" "
    SELECT count(*) FROM macro_price_bars WHERE quality_status='STALE' AND volume > 0" "0"

check "no traded row is stored with zero volume and a frozen price" "
    SELECT count(*) FROM macro_price_bars
    WHERE quality_status='VALID' AND volume = 0
      AND open = high AND high = low AND low = close" "0"

echo
if [[ $fails -eq 0 ]]; then echo "schema invariants: PASS"; else echo "schema invariants: $fails FAILED"; fi
exit $fails
