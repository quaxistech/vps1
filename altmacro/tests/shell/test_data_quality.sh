#!/usr/bin/env bash
# Historical quality validation over whatever is stored, plus a fixed set of
# verified samples.
#
# The samples exist so that a change in decoder semantics cannot pass quietly.
# Each was read from the live source on 2026-09-10 and cross-checked against an
# independent publisher: the offshore CNH quote against the Federal Reserve's
# H.10 reference, the gold quote against the LBMA auction. If a future decoder
# shifts a scale or swaps a field, these stop matching.
#
#   sudo -u postgres ./test_data_quality.sh [dbname]
set -euo pipefail
DB="${1:-altmacro}"
fails=0

q() { psql -X -q -t -A -d "$DB" -c "$1" | tr -d '[:space:]'; }

expect_zero() {
    local name="$1" sql="$2" got
    got="$(q "$sql")"
    if [[ "$got" == "0" ]]; then
        echo "  ok    $name"
    else
        echo "  FAIL  $name ($got offending rows)"
        fails=$((fails + 1))
    fi
}

echo "quality validation on $DB"

expect_zero "prices are strictly positive" "
    SELECT count(*) FROM macro_price_bars
    WHERE quality_status IN ('VALID','STALE') AND (open<=0 OR high<=0 OR low<=0 OR close<=0)"

expect_zero "low <= open,close <= high" "
    SELECT count(*) FROM macro_price_bars
    WHERE quality_status IN ('VALID','STALE')
      AND NOT (high>=open AND high>=close AND high>=low AND low<=open AND low<=close)"

expect_zero "volume is never negative" "
    SELECT count(*) FROM macro_price_bars WHERE volume < 0"

expect_zero "no duplicated observations" "
    SELECT count(*) FROM (
        SELECT instrument_id, provider_id, timeframe, price_basis, event_time
        FROM macro_price_bars GROUP BY 1,2,3,4,5 HAVING count(*)>1) d"

expect_zero "no bar falls outside its instrument's sanity band" "
    SELECT count(*) FROM macro_price_bars b
    JOIN macro_instrument_sources s
      ON s.instrument_id=b.instrument_id AND s.provider_id=b.provider_id
     AND s.price_basis=b.price_basis
    WHERE b.quality_status IN ('VALID','STALE') AND s.sanity_min IS NOT NULL
      AND (b.close < s.sanity_min OR b.close > s.sanity_max)"

expect_zero "no reference value falls outside its band" "
    SELECT count(*) FROM macro_reference_values v
    JOIN macro_instrument_sources s
      ON s.instrument_id=v.instrument_id AND s.provider_id=v.provider_id
    WHERE v.quality_status='VALID' AND s.sanity_min IS NOT NULL
      AND (v.value < s.sanity_min OR v.value > s.sanity_max)"

expect_zero "no bar is timestamped in the future" "
    SELECT count(*) FROM macro_price_bars WHERE event_time > now() + interval '1 hour'"

expect_zero "ingest never precedes the event it recorded by more than a minute" "
    SELECT count(*) FROM macro_price_bars WHERE ingest_time < event_time - interval '1 minute'"

# bid <= ask, checked where both sides exist for the same instant.
expect_zero "the book is never crossed" "
    SELECT count(*) FROM macro_price_bars a
    JOIN macro_price_bars b
      ON b.instrument_id=a.instrument_id AND b.provider_id=a.provider_id
     AND b.timeframe=a.timeframe AND b.event_time=a.event_time
     AND a.price_basis='bid' AND b.price_basis='ask'
    WHERE a.quality_status IN ('VALID','STALE') AND b.quality_status IN ('VALID','STALE')
      AND a.close > b.close"

expect_zero "no zero-volume flat bar is stored as a traded observation" "
    SELECT count(*) FROM macro_price_bars
    WHERE quality_status='VALID' AND volume = 0
      AND open = high AND high = low AND low = close"

expect_zero "no carried-forward bar claims volume" "
    SELECT count(*) FROM macro_price_bars WHERE quality_status='STALE' AND volume > 0"

# The three CNY/CNH semantics must remain distinguishable in the stored data.
echo "==> CNY/CNH separation"
common="$(q "SELECT count(*) FROM (
    SELECT v.event_time FROM macro_reference_values v
    JOIN macro_instruments i ON i.id=v.instrument_id
    WHERE i.code IN ('REF.USDCNY.FED','REF.USDCNY.PBOC') AND v.value IS NOT NULL
    GROUP BY 1 HAVING count(*)=2) x")"
identical="$(q "SELECT count(*) FROM (
    SELECT v.event_time,
           max(v.value) FILTER (WHERE i.code='REF.USDCNY.FED') AS fed,
           max(v.value) FILTER (WHERE i.code='REF.USDCNY.PBOC') AS pboc
    FROM macro_reference_values v JOIN macro_instruments i ON i.id=v.instrument_id
    WHERE i.code IN ('REF.USDCNY.FED','REF.USDCNY.PBOC') AND v.value IS NOT NULL
    GROUP BY 1 HAVING count(*)=2) x WHERE fed = pboc")"
echo "    ${common} shared dates, ${identical} of them identical"
if [[ "$common" -lt 20 ]]; then
    echo "  WARN  too few shared dates to judge separation yet"
elif [[ "$identical" -eq "$common" ]]; then
    echo "  FAIL  the official reference and the fixing hold the same number on every date;"
    echo "        they have been merged somewhere"
    fails=$((fails + 1))
else
    echo "  ok    the official reference and the fixing are distinct series"
fi

echo "==> verified samples"
sample() {
    local label="$1" sql="$2" expect="$3" got
    got="$(q "$sql")"
    if [[ -z "$got" ]]; then
        echo "  SKIP  $label (not loaded yet)"
    elif [[ "$got" == "$expect" ]]; then
        echo "  ok    $label = $got"
    else
        echo "  FAIL  $label: expected $expect, stored $got"
        fails=$((fails + 1))
    fi
}

# Federal Reserve H.10, USD/CNY, 2026-09-04. Read from fredgraph.csv.
sample "H.10 USD/CNY on 2026-09-04" "
    SELECT round(v.value,4)::text FROM macro_reference_values v
    JOIN macro_instruments i ON i.id=v.instrument_id
    WHERE i.code='REF.USDCNY.FED' AND v.event_time='2026-09-04'::date" "6.7108"

# PBOC central parity for the same currency pair, 2026-09-10. Confirmed against
# chinamoney.com.cn first-hand on the same day: the two agreed exactly.
sample "PBOC central parity on 2026-09-10" "
    SELECT round(v.value,4)::text FROM macro_reference_values v
    JOIN macro_instruments i ON i.id=v.instrument_id
    WHERE i.code='REF.USDCNY.PBOC' AND v.event_time='2026-09-10'::date" "6.7766"

# VIX close, 2026-09-09, from FRED. Independently matches Cboe's own history file.
sample "VIX close on 2026-09-09" "
    SELECT round(v.value,2)::text FROM macro_reference_values v
    JOIN macro_instruments i ON i.id=v.instrument_id
    WHERE i.code='IDX.VIX' AND v.event_time='2026-09-09'::date" "16.46"

# The first minute of the target window, decoded from Dukascopy's .bi5 archive.
# A wrong scale or a swapped field changes this.
sample "USDCNH bid open, 2025-12-01 00:00 UTC" "
    SELECT round(b.open,5)::text FROM macro_price_bars b
    JOIN macro_instruments i ON i.id=b.instrument_id
    WHERE i.code='FX.USDCNH' AND b.price_basis='bid'
      AND b.event_time='2025-12-01 00:00:00+00'" "7.06801"

sample "XAUUSD bid open, 2025-12-01 00:00 UTC" "
    SELECT round(b.open,3)::text FROM macro_price_bars b
    JOIN macro_instruments i ON i.id=b.instrument_id
    WHERE i.code='METAL.XAUUSD' AND b.price_basis='bid'
      AND b.event_time='2025-12-01 00:00:00+00'" "4221.655"

echo
if [[ $fails -eq 0 ]]; then echo "data quality: PASS"; else echo "data quality: $fails FAILED"; fi
exit $fails
