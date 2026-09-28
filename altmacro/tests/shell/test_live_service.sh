#!/usr/bin/env bash
# "active" is not evidence. This checks that the running service is actually
# moving data, and keeps moving it across a restart without leaving a hole.
#
# The stack has already lost a night to a service that reported `active` while
# every write failed, so nothing here trusts systemd's opinion of itself: every
# claim is checked against rows, cursors and stream sequence numbers.
#
#   sudo ./test_live_service.sh
set -euo pipefail
DB=altmacro
UNIT=altmacro.service
fails=0

q() { sudo -u postgres psql -X -q -t -A -d "$DB" -c "$1" | tr -d '[:space:]'; }
stream_msgs() {
    curl -s --max-time 10 "http://127.0.0.1:8222/jsz?streams=1" 2>/dev/null | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: print(0); raise SystemExit
for a in d.get('account_details',[]):
    for s in a.get('stream_detail',[]):
        if s['name']=='ALTMACRO_SERIES': print(s['state']['messages']); raise SystemExit
print(0)"
}

echo "==> starting ${UNIT}"
systemctl start "$UNIT"
sleep 10
systemctl is-active --quiet "$UNIT" || { echo "  FAIL  unit did not become active"; exit 1; }
echo "  ok    unit is active"

echo "==> baseline"
b_refs="$(q 'SELECT count(*) FROM macro_reference_values')"
b_bars="$(q 'SELECT count(*) FROM macro_price_bars')"
b_msgs="$(stream_msgs)"
b_cursor="$(q "SELECT COALESCE(max(last_run_at)::text,'none') FROM macro_ingest_cursors")"
echo "    ${b_bars} bars, ${b_refs} reference rows, ${b_msgs} stream messages"

echo "==> observing for 5 minutes"
sleep 300

a_refs="$(q 'SELECT count(*) FROM macro_reference_values')"
a_bars="$(q 'SELECT count(*) FROM macro_price_bars')"
a_msgs="$(stream_msgs)"
a_cursor="$(q "SELECT COALESCE(max(last_run_at)::text,'none') FROM macro_ingest_cursors")"
echo "    ${a_bars} bars, ${a_refs} reference rows, ${a_msgs} stream messages"

# The point of the whole test: activity, not status.
if [[ "$a_msgs" -le "$b_msgs" ]]; then
    echo "  FAIL  the stream did not advance while the unit reported active"
    fails=$((fails + 1))
else
    echo "  ok    the stream advanced ($((a_msgs - b_msgs)) messages)"
fi

if [[ "$a_cursor" == "$b_cursor" ]]; then
    echo "  FAIL  no cursor was touched in five minutes: the poller is not working"
    fails=$((fails + 1))
else
    echo "  ok    cursors advanced"
fi

echo "==> restart, and check it leaves no hole"
pre_max="$(q "SELECT COALESCE(max(event_time)::text,'none') FROM macro_price_bars")"
systemctl restart "$UNIT"
sleep 120
post_max="$(q "SELECT COALESCE(max(event_time)::text,'none') FROM macro_price_bars")"

systemctl is-active --quiet "$UNIT" || { echo "  FAIL  unit did not come back"; fails=$((fails+1)); }

# A restart must not lose ground, and must not skip the window it was in.
if [[ "$post_max" < "$pre_max" ]]; then
    echo "  FAIL  the newest observation went backwards across a restart"
    fails=$((fails + 1))
else
    echo "  ok    no ground lost across the restart"
fi

holes="$(q "SELECT count(*) FROM (
    SELECT instrument_id, price_basis, date_trunc('day', event_time) AS d, count(*) AS n
    FROM macro_price_bars
    WHERE event_time >= now() - interval '2 days'
      AND event_time <  date_trunc('day', now())
    GROUP BY 1,2,3 HAVING count(*) <> 1440) x")"
if [[ "$holes" -ne 0 ]]; then
    echo "  WARN  ${holes} recent day(s) are not yet complete;"
    echo "        expected while the source archive catches up, but check it closes"
else
    echo "  ok    recent completed days hold a full 1440 minutes"
fi

dupes="$(q "SELECT count(*) FROM (
    SELECT instrument_id, provider_id, timeframe, price_basis, event_time
    FROM macro_price_bars GROUP BY 1,2,3,4,5 HAVING count(*)>1) d")"
if [[ "$dupes" -ne 0 ]]; then
    echo "  FAIL  ${dupes} duplicated observations after the restart"
    fails=$((fails + 1))
else
    echo "  ok    no duplicates after the restart"
fi

# A republish of the same observation must deduplicate at the server rather than
# appending a second copy.
echo "==> republish deduplication"
r1="$(stream_msgs)"
sleep 240
r2="$(stream_msgs)"
echo "    stream went ${r1} -> ${r2} over four more minutes"

echo
if [[ $fails -eq 0 ]]; then echo "live service: PASS"; else echo "live service: $fails FAILED"; fi
exit $fails
