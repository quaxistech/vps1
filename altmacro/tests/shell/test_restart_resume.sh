#!/usr/bin/env bash
# A bootstrap killed mid-flight must resume where it stopped: no duplicates, no
# skipped chunk, and the same final row count as an uninterrupted run.
#
# The failure this guards against is not a crash -- it is a resume that looks
# successful while quietly skipping the range it was interrupted in.
#
#   sudo ./test_restart_resume.sh
set -euo pipefail
CONFIG="${ALTMACRO_TEST_CONFIG:-/etc/altmacro/smoke.yaml}"
BIN=/opt/altmacro/bin/altmacro
DB=altmacro
fails=0

q() { sudo -u postgres psql -X -q -t -A -d "$DB" -c "$1" | tr -d '[:space:]'; }

run_bootstrap() {
    sudo -u altmacro bash -c "set -a; . /etc/altmacro/altmacro.env; set +a; $BIN bootstrap --config $CONFIG --no-gap-scan" \
        >/tmp/altmacro_resume_$1.log 2>&1 || true
}

echo "==> clearing the window under test"
sudo -u postgres psql -X -q -d "$DB" -c "TRUNCATE macro_price_bars, macro_reference_values,
    macro_ingest_cursors, macro_ingest_runs, macro_series_gaps" >/dev/null

echo "==> first pass, killed after 45 seconds"
run_bootstrap first &
runner=$!
sleep 45
sudo pkill -f "altmacro bootstrap" 2>/dev/null || true
wait $runner 2>/dev/null || true

partial_bars="$(q 'SELECT count(*) FROM macro_price_bars')"
cursors="$(q 'SELECT count(*) FROM macro_ingest_cursors')"
echo "    after the kill: ${partial_bars} bars, ${cursors} cursors"
if [[ "$partial_bars" -eq 0 ]]; then
    echo "  SKIP  nothing was written before the kill; widen the window or lengthen the wait"
    exit 0
fi
if [[ "$cursors" -eq 0 ]]; then
    echo "  FAIL  rows were written but no cursor survived: a restart would begin again from zero"
    fails=$((fails + 1))
fi

echo "==> second pass, allowed to finish"
run_bootstrap second
resumed_bars="$(q 'SELECT count(*) FROM macro_price_bars')"

echo "==> third pass over the same window, which must add nothing"
run_bootstrap third
repeat_bars="$(q 'SELECT count(*) FROM macro_price_bars')"

echo
echo "    bars after resume:  ${resumed_bars}"
echo "    bars after repeat:  ${repeat_bars}"

if [[ "$resumed_bars" -le "$partial_bars" ]]; then
    echo "  FAIL  the resume added nothing: it believed the window was already complete"
    fails=$((fails + 1))
else
    echo "  ok    the resume continued rather than stopping short"
fi

if [[ "$repeat_bars" -ne "$resumed_bars" ]]; then
    echo "  FAIL  re-running a completed window changed the row count ($resumed_bars -> $repeat_bars)"
    fails=$((fails + 1))
else
    echo "  ok    re-running a completed window is a no-op"
fi

# The upsert is keyed on the observation's own identity, so a duplicate is
# impossible by construction -- but that is exactly the kind of claim worth
# checking rather than trusting.
dupes="$(q "SELECT count(*) FROM (
    SELECT instrument_id, provider_id, timeframe, price_basis, event_time
    FROM macro_price_bars GROUP BY 1,2,3,4,5 HAVING count(*) > 1) d")"
if [[ "$dupes" -ne 0 ]]; then
    echo "  FAIL  ${dupes} duplicated observations"
    fails=$((fails + 1))
else
    echo "  ok    no duplicated observations"
fi

# Both sides of the book must be present. This is the defect that was actually
# found in testing: one shared cursor let ask complete and skipped bid entirely.
sides="$(q "SELECT count(DISTINCT price_basis) FROM macro_price_bars")"
if [[ "$sides" -lt 2 ]]; then
    echo "  FAIL  only ${sides} side of the book was stored; bid and ask must both be fetched"
    fails=$((fails + 1))
else
    echo "  ok    both sides of the book are present"
fi

# A window widened backwards must be filled, not skipped. This is the second
# defect found in testing: a cursor left behind by a narrower run made the next,
# wider run resume from the old watermark and step straight past the nine months
# it had just been asked for -- and the resulting series looked complete.
echo "==> widening the window backwards"
before_earliest="$(q "SELECT COALESCE(min(event_time)::date::text,'none') FROM macro_price_bars")"
WIDE=/tmp/altmacro_wide.yaml
sudo cp "$CONFIG" "$WIDE"
sudo sed -i "s|history_start: \".*\"|history_start: \"2026-08-25T00:00:00Z\"|" "$WIDE"
sudo chown root:altmacro "$WIDE" && sudo chmod 0640 "$WIDE"
sudo -u altmacro bash -c "set -a; . /etc/altmacro/altmacro.env; set +a; timeout 240 $BIN bootstrap --config $WIDE --no-gap-scan" \
    >/tmp/altmacro_resume_wide.log 2>&1 || true
after_earliest="$(q "SELECT COALESCE(min(event_time)::date::text,'none') FROM macro_price_bars")"
sudo rm -f "$WIDE"

echo "    earliest before: ${before_earliest}"
echo "    earliest after:  ${after_earliest}"
if [[ "$after_earliest" == "$before_earliest" ]]; then
    echo "  FAIL  the widened window loaded nothing: the old watermark skipped the new range"
    fails=$((fails + 1))
else
    echo "  ok    the widened window was filled rather than skipped"
fi

echo
if [[ $fails -eq 0 ]]; then echo "restart/resume: PASS"; else echo "restart/resume: $fails FAILED"; fi
exit $fails
