#!/usr/bin/env bash
# One pass along the whole chain, from what a provider returned to what AltTech
# can see. Every claim is a query against live state; nothing is inferred from
# configuration or from a service's opinion of itself.
#
#   INPUT -> provider -> decoder -> normalisation -> validation -> DB write
#         -> checkpoint -> coverage metadata -> NATS -> AltTech visibility
#
#   sudo ./forensic_audit.sh [label]
set -euo pipefail
LABEL="${1:-audit}"
DB=altmacro
fails=0
warns=0

q() { sudo -u postgres psql -X -q -t -A -d "$DB" -c "$1" | tr -d '[:space:]'; }
qa() { sudo -u postgres psql -X -q -t -A -d "$2" -c "$1" | tr -d '[:space:]'; }

must_be() {
    local name="$1" got="$2" want="$3"
    if [[ "$got" == "$want" ]]; then echo "  ok    $name"
    else echo "  FAIL  $name (got '$got', want '$want')"; fails=$((fails+1)); fi
}
note() { echo "  ..    $1"; }
warn() { echo "  WARN  $1"; warns=$((warns+1)); }

echo "==================================================================="
echo " altmacro forensic audit — ${LABEL} — $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "==================================================================="

echo
echo "-- 1. decoder and normalisation --------------------------------"
must_be "no bar violates OHLC ordering" \
    "$(q "SELECT count(*) FROM macro_price_bars WHERE close IS NOT NULL
          AND NOT (high>=open AND high>=close AND high>=low AND low<=open AND low<=close)")" "0"
must_be "no stored price is zero or negative" \
    "$(q "SELECT count(*) FROM macro_price_bars
          WHERE open<=0 OR high<=0 OR low<=0 OR close<=0")" "0"
must_be "no price escapes its instrument's sanity band" \
    "$(q "SELECT count(*) FROM macro_price_bars b
          JOIN macro_instrument_sources s ON s.instrument_id=b.instrument_id
           AND s.provider_id=b.provider_id AND s.price_basis=b.price_basis
          WHERE b.close IS NOT NULL AND s.sanity_min IS NOT NULL
            AND (b.close < s.sanity_min OR b.close > s.sanity_max)")" "0"
must_be "every bar is aligned to a whole minute" \
    "$(q "SELECT count(*) FROM macro_price_bars
          WHERE EXTRACT(SECOND FROM event_time) <> 0")" "0"
must_be "no bar is dated in the future" \
    "$(q "SELECT count(*) FROM macro_price_bars WHERE event_time > now() + interval '1 hour'")" "0"

echo
echo "-- 2. absence is never a number --------------------------------"
must_be "no absent bar carries a price" \
    "$(q "SELECT count(*) FROM macro_price_bars
          WHERE quality_status NOT IN ('VALID','STALE','PARTIAL') AND close IS NOT NULL")" "0"
must_be "no absent reference row carries a value" \
    "$(q "SELECT count(*) FROM macro_reference_values
          WHERE quality_status NOT IN ('VALID','STALE') AND value IS NOT NULL")" "0"
must_be "no carried-forward bar is labelled as traded" \
    "$(q "SELECT count(*) FROM macro_price_bars WHERE quality_status='VALID' AND volume=0
            AND open=high AND high=low AND low=close")" "0"
must_be "no carried-forward bar claims volume" \
    "$(q "SELECT count(*) FROM macro_price_bars WHERE quality_status='STALE' AND volume>0")" "0"

echo
echo "-- 3. duplicates and ordering ----------------------------------"
must_be "no duplicated bar observation" \
    "$(q "SELECT count(*) FROM (SELECT instrument_id,provider_id,timeframe,price_basis,event_time
          FROM macro_price_bars GROUP BY 1,2,3,4,5 HAVING count(*)>1) d")" "0"
must_be "no duplicated reference observation" \
    "$(q "SELECT count(*) FROM (SELECT instrument_id,provider_id,event_time
          FROM macro_reference_values GROUP BY 1,2,3 HAVING count(*)>1) d")" "0"
must_be "the book is never crossed" \
    "$(q "SELECT count(*) FROM macro_price_bars a
          JOIN macro_price_bars b ON b.instrument_id=a.instrument_id
           AND b.provider_id=a.provider_id AND b.timeframe=a.timeframe
           AND b.event_time=a.event_time AND a.price_basis='bid' AND b.price_basis='ask'
          WHERE a.close IS NOT NULL AND b.close IS NOT NULL AND a.close > b.close")" "0"

echo
echo "-- 4. coverage and continuity ----------------------------------"
note "per-series coverage against the requested window"
# Aggregated once per day, then summarised. The obvious shape -- a LATERAL that
# counts a day's rows for each row in it -- is quadratic and took 75 minutes on
# this table while holding locks behind it.
sudo -u postgres psql -X -q -d "$DB" -P pager=off -c "
WITH per_day AS (
    SELECT instrument_id, price_basis, date_trunc('day', event_time) AS d, count(*) AS n
    FROM macro_price_bars GROUP BY 1,2,3)
SELECT i.code, p.price_basis, count(*) AS days,
       count(*) FILTER (WHERE p.n <> 1440) AS incomplete_days,
       min(p.d)::date AS earliest, max(p.d)::date AS latest
FROM per_day p JOIN macro_instruments i ON i.id = p.instrument_id
GROUP BY 1,2 ORDER BY 1,2" 2>&1 | sed 's/^/    /'

must_be "every completed day holds a full 1440 minutes" \
    "$(q "SELECT count(*) FROM (
          SELECT instrument_id, price_basis, date_trunc('day',event_time) AS d, count(*) AS n
          FROM macro_price_bars
          WHERE event_time < date_trunc('day', (SELECT max(event_time) FROM macro_price_bars))
          GROUP BY 1,2,3 HAVING count(*) <> 1440) x")" "0"
must_be "both sides of the book are present for every market instrument" \
    "$(q "SELECT count(*) FROM (
          SELECT instrument_id FROM macro_price_bars GROUP BY 1
          HAVING count(DISTINCT price_basis) < 2) x")" "0"

# The configured window start must track AltSpotter's real retention horizon.
# It is a value in a file, and AltSpotter's horizon moves, so the two are
# compared here rather than assumed to agree: a silent divergence would leave
# external series covering different ground from the crypto they are studied
# against, and nothing else would notice.
spotter="$(qa "SELECT min(open_time)::date::text FROM market_candles_1m" altspotter 2>/dev/null || echo unknown)"
macro_cfg="$(sed -n 's/.*history_start:[[:space:]]*"\?\([0-9-]\{10\}\).*/\1/p' /etc/altmacro/altmacro.yaml | head -1)"
macro_data="$(q "SELECT min(event_time)::date::text FROM macro_price_bars")"
note "AltSpotter history begins ${spotter}; altmacro is configured from ${macro_cfg} and holds from ${macro_data}"
must_be "the configured window start matches AltSpotter's horizon" "$macro_cfg" "$spotter"
must_be "stored history reaches the configured start" "$macro_data" "$macro_cfg"

echo
echo "-- 5. checkpoints ----------------------------------------------"
must_be "every cursor records where its walk began" \
    "$(q "SELECT count(*) FROM macro_ingest_cursors WHERE backfilled_from IS NULL")" "0"
must_be "no watermark precedes the walk that set it" \
    "$(q "SELECT count(*) FROM macro_ingest_cursors WHERE watermark < backfilled_from")" "0"
must_be "every cursor is backed by rows that actually committed" \
    "$(q "SELECT count(*) FROM macro_ingest_cursors c WHERE c.timeframe='1m'
            AND NOT EXISTS (SELECT 1 FROM macro_price_bars b
                            WHERE b.instrument_id=c.instrument_id
                              AND b.provider_id=c.provider_id
                              AND b.price_basis=c.price_basis)")" "0"
# The most dangerous form of false coverage, because no later run can notice:
# a watermark that sits past a period the source had not archived yet. The next
# run starts after the hole, so the gap is permanent while the cursor says the
# window is complete.
must_be "no watermark sits beyond a period the source has not archived yet" \
    "$(q "SELECT count(*) FROM macro_ingest_cursors c
          JOIN macro_series_gaps g ON g.instrument_id=c.instrument_id
           AND g.provider_id=c.provider_id AND g.timeframe=c.timeframe
          WHERE g.resolution='provider_delay' AND g.resolved_at IS NULL
            AND c.watermark > g.gap_start")" "0"
must_be "the cursor key separates bid from ask" \
    "$(q "SELECT (count(*) > 0)::int FROM pg_index i JOIN pg_class c ON c.oid=i.indrelid
          JOIN pg_attribute a ON a.attrelid=c.oid AND a.attnum=ANY(i.indkey)
          WHERE c.relname='macro_ingest_cursors' AND i.indisprimary AND a.attname='price_basis'")" "1"

echo
echo "-- 6. gap classification ---------------------------------------"
note "recorded gaps by resolution"
sudo -u postgres psql -X -q -d "$DB" -P pager=off -c "
SELECT i.code, g.resolution, count(*) AS n, min(g.gap_start)::date AS f, max(g.gap_start)::date AS t
FROM macro_series_gaps g JOIN macro_instruments i ON i.id=g.instrument_id
GROUP BY 1,2 ORDER BY 1,2" 2>&1 | sed 's/^/    /'
must_be "no weekday is filed as a closed market" \
    "$(q "SELECT count(*) FROM macro_series_gaps
          WHERE resolution='market_closed' AND EXTRACT(ISODOW FROM gap_start) BETWEEN 1 AND 5")" "0"

echo
echo "-- 7. source semantics are not merged --------------------------"
must_be "CNY/CNH exist as three separate instruments" \
    "$(q "SELECT count(*) FROM macro_instruments
          WHERE code IN ('FX.USDCNH','REF.USDCNY.FED','REF.USDCNY.PBOC')")" "3"
identical="$(q "SELECT count(*) FROM (
    SELECT v.event_time,
           max(v.value) FILTER (WHERE i.code='REF.USDCNY.FED')  AS fed,
           max(v.value) FILTER (WHERE i.code='REF.USDCNY.PBOC') AS pboc
    FROM macro_reference_values v JOIN macro_instruments i ON i.id=v.instrument_id
    WHERE i.code IN ('REF.USDCNY.FED','REF.USDCNY.PBOC') AND v.value IS NOT NULL
    GROUP BY 1 HAVING count(*)=2) x WHERE fed=pboc")"
must_be "the official reference and the fixing never coincide" "$identical" "0"
must_be "each instrument keeps one denomination" \
    "$(q "SELECT count(*) FROM (SELECT instrument_id FROM macro_instrument_sources s
          JOIN macro_instruments i ON i.id=s.instrument_id
          GROUP BY 1 HAVING count(DISTINCT i.denomination) > 1) x")" "0"

echo
echo "-- 8. look-ahead -----------------------------------------------"
must_be "no observation was ingested before it happened" \
    "$(q "SELECT count(*) FROM macro_price_bars
          WHERE ingest_time < event_time - interval '1 minute'")" "0"
must_be "no reference value claims publication before observation" \
    "$(q "SELECT count(*) FROM macro_reference_values
          WHERE publication_time IS NOT NULL AND publication_time < event_time")" "0"
# The poller re-reads its window every pass. If a re-read stamped the row as
# freshly ingested, every observation would look as new as the last poll and
# ingest_time would stop bounding when the number was first knowable here.
# Each query answers 1 only when re-read rows exist and none kept an older
# ingest_time: rows first seen and re-read within one poll interval (a
# restart's backfill overlapping the first live pass) do not trip it.
must_be "a re-read reference value keeps its first ingest_time" \
    "$(q "SELECT count(*) FROM (SELECT 1 FROM macro_reference_values
          WHERE revision > 0 AND updated_at > now() - interval '24 hours'
          HAVING count(*) > 0
             AND count(*) FILTER (WHERE updated_at - ingest_time >= interval '10 minutes') = 0) x")" "0"
must_be "a re-read bar keeps its first ingest_time" \
    "$(q "SELECT count(*) FROM (SELECT 1 FROM macro_price_bars
          WHERE revision > 0 AND updated_at > now() - interval '24 hours'
          HAVING count(*) > 0
             AND count(*) FILTER (WHERE updated_at - ingest_time >= interval '10 minutes') = 0) x")" "0"
must_be "every release vintage is dated" \
    "$(q "SELECT count(*) FROM macro_events WHERE vintage_date IS NULL")" "0"

echo
echo "-- 9. provider health ------------------------------------------"
sudo -u postgres psql -X -q -d "$DB" -P pager=off -c "
SELECT p.code, m.consecutive_failures AS fails, m.last_success_at::timestamp(0) AS last_ok,
       left(coalesce(m.last_failure_reason,''),40) AS last_failure
FROM macro_provider_metadata m JOIN macro_providers p ON p.id=m.provider_id
WHERE p.enabled ORDER BY p.id" 2>&1 | sed 's/^/    /'
must_be "no documented rate limit is uncited" \
    "$(q "SELECT count(*) FROM macro_providers
          WHERE rate_limit_source='documented' AND rate_limit_ref IS NULL")" "0"

echo
echo "-- 10. NATS ----------------------------------------------------"
msgs="$(curl -s --max-time 10 'http://127.0.0.1:8222/jsz?streams=1' 2>/dev/null | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: print('0 0 0'); raise SystemExit
for a in d.get('account_details',[]):
    for s in a.get('stream_detail',[]):
        if s['name']=='ALTMACRO_SERIES':
            st=s['state']; print(st['messages'], st['bytes'], st['last_seq']); raise SystemExit
print('0 0 0')")"
note "stream ALTMACRO_SERIES: $(echo "$msgs" | awk '{print $1" messages, "$2" bytes, last_seq "$3}')"
must_be "the stream exists and holds messages" \
    "$([[ "$(echo "$msgs" | awk '{print $1}')" -gt 0 ]] && echo yes || echo no)" "yes"
leftover="$(curl -s --max-time 10 'http://127.0.0.1:8222/jsz?streams=1&consumers=1' 2>/dev/null | \
  grep -c 'altmacro_verify_' || true)"
must_be "no temporary verification consumer was left behind" "$leftover" "0"

echo
echo "-- 11. AltTech visibility --------------------------------------"
vis="$(qa "SELECT count(*) FROM altmacro_ref.macro_instruments" alttech 2>/dev/null || echo ERR)"
must_be "alttech can read the instrument registry" \
    "$([[ "$vis" =~ ^[0-9]+$ && "$vis" -gt 0 ]] && echo yes || echo no)" "yes"
# The predicate matters: `WHERE false` is optimised away and never reaches the
# foreign table, so it proves nothing. This one forces the remote statement.
ro="$(sudo -u postgres psql -X -q -t -A -d alttech -c \
    "SET ROLE alttech_app; UPDATE altmacro_ref.macro_instruments SET display_name = display_name WHERE id = 1;" 2>&1 | \
    grep -ci 'permission denied' || true)"
must_be "the foreign schema is read-only to alttech" \
    "$([[ "$ro" -ge 1 ]] && echo yes || echo no)" "yes"

# `active` is not evidence of anything on its own. A service killed by its own
# watchdog every two minutes reports `active` continuously while getting no
# further than its first binding -- which is exactly what happened here,
# nineteen restarts in a row, before the heartbeat was moved inside the waits.
restarts="$(systemctl show altmacro.service -p NRestarts --value 2>/dev/null || echo 0)"
uptime_s="$(( $(date +%s) - $(date -d "$(systemctl show altmacro.service -p ActiveEnterTimestamp --value)" +%s 2>/dev/null || date +%s) ))"
note "altmacro.service: ${restarts} restarts, up ${uptime_s}s"
# Counted since this instance started, not over a fixed window: a watchdog kill
# restarts the service, so anything before ActiveEnterTimestamp belongs to an
# instance that no longer exists and would make the check remember old history
# forever.
since="$(systemctl show altmacro.service -p ActiveEnterTimestamp --value)"
must_be "the running service is not being killed by its watchdog" \
    "$(journalctl -u altmacro.service --since "$since" --no-pager 2>/dev/null | grep -c "result 'watchdog'" || true)" "0"

echo
echo "-- 12. deployment and secrets ----------------------------------"
must_be "one installed tree: no releases/ directory" "$([[ -e /opt/altmacro/releases ]] && echo present || echo absent)" "absent"
must_be "one installed tree: no current symlink" "$([[ -e /opt/altmacro/current ]] && echo present || echo absent)" "absent"
must_be "the running binary is /opt/altmacro/bin/altmacro" \
    "$(readlink -f /proc/$(systemctl show -p MainPID --value altmacro.service)/exe)" "/opt/altmacro/bin/altmacro"
must_be "no credential appears in the service journal" \
    "$(journalctl -u altmacro.service -u altmacro-bootstrap.service --no-pager 2>/dev/null | \
       grep -cE 'nats://[^:]+:[^@<]+@|ALTMACRO_DB_PASSWORD=.' || true)" "0"
must_be "the secret file is not world-readable" \
    "$(stat -c '%a' /etc/altmacro/altmacro.env)" "640"
must_be "no password sits in the YAML configuration" \
    "$(grep -ciE '^[[:space:]]*(password|token|api_key|secret):' /etc/altmacro/altmacro.yaml || true)" "0"

echo
echo "==================================================================="
if [[ $fails -eq 0 ]]; then
    echo " ${LABEL}: NO REAL DEFECTS  (${warns} warning(s))"
else
    echo " ${LABEL}: ${fails} DEFECT(S), ${warns} warning(s)"
fi
echo "==================================================================="
exit $fails
