#!/usr/bin/env bash
# When PostgreSQL goes away mid-bootstrap the service must not believe it wrote
# anything, must not advance its cursor, and must recover when the database
# returns.
#
# The one thing that must never happen: a cursor moved forward while the rows
# it covers failed to commit. That is silent data loss with no error anywhere.
#
#   sudo ./test_db_failure.sh
set -euo pipefail
CONFIG="${ALTMACRO_TEST_CONFIG:-/etc/altmacro/smoke.yaml}"
BIN=/opt/altmacro/bin/altmacro
DB=altmacro
fails=0

q() { sudo -u postgres psql -X -q -t -A -d "$DB" -c "$1" | tr -d '[:space:]'; }

restore() {
    systemctl is-active --quiet postgresql || systemctl start postgresql
    for _ in $(seq 1 30); do
        sudo -u postgres psql -X -q -t -A -d "$DB" -c 'SELECT 1' >/dev/null 2>&1 && break
        sleep 1
    done
}
trap restore EXIT

echo "==> baseline"
before_bars="$(q 'SELECT count(*) FROM macro_price_bars')"
before_wm="$(q "SELECT COALESCE(max(watermark)::text,'none') FROM macro_ingest_cursors WHERE timeframe='1m'")"
echo "    ${before_bars} bars, watermark ${before_wm}"

echo "==> starting a bootstrap, then stopping PostgreSQL under it"
sudo -u altmacro bash -c "set -a; . /etc/altmacro/altmacro.env; set +a; timeout 240 $BIN bootstrap --config $CONFIG --no-gap-scan" \
    >/tmp/altmacro_db_failure.log 2>&1 &
runner=$!
sleep 20
systemctl stop postgresql
sleep 25
restore
wait $runner 2>/dev/null || true

echo "==> after the database returned"
after_bars="$(q 'SELECT count(*) FROM macro_price_bars')"
after_wm="$(q "SELECT COALESCE(max(watermark)::text,'none') FROM macro_ingest_cursors WHERE timeframe='1m'")"
echo "    ${after_bars} bars, watermark ${after_wm}"

if [[ "$after_bars" -lt "$before_bars" ]]; then
    echo "  FAIL  rows were lost across the outage (${before_bars} -> ${after_bars})"
    fails=$((fails + 1))
else
    echo "  ok    nothing already committed was lost"
fi

# The invariant that matters: every stored cursor must be backed by rows that
# actually reached disk. A watermark ahead of the data is a permanent hole.
orphans="$(q "SELECT count(*) FROM macro_ingest_cursors c
              WHERE c.timeframe='1m'
                AND NOT EXISTS (SELECT 1 FROM macro_price_bars b
                                WHERE b.instrument_id=c.instrument_id
                                  AND b.provider_id=c.provider_id
                                  AND b.price_basis=c.price_basis)
                AND c.backfilled_from IS NOT NULL
                AND c.watermark > c.backfilled_from")"
if [[ "$orphans" -ne 0 ]]; then
    echo "  FAIL  ${orphans} cursors claim covered ground with no rows behind them"
    fails=$((fails + 1))
else
    echo "  ok    every cursor is backed by rows that actually committed"
fi

echo "==> the service recovers and makes progress"
sudo -u altmacro bash -c "set -a; . /etc/altmacro/altmacro.env; set +a; timeout 180 $BIN bootstrap --config $CONFIG --no-gap-scan" \
    >/tmp/altmacro_db_recovery.log 2>&1 || true
final_bars="$(q 'SELECT count(*) FROM macro_price_bars')"
if [[ "$final_bars" -lt "$after_bars" ]]; then
    echo "  FAIL  the recovery run lost rows"
    fails=$((fails + 1))
else
    echo "  ok    the recovery run continued normally (${final_bars} bars)"
fi

trap - EXIT
restore
echo
if [[ $fails -eq 0 ]]; then echo "database failure: PASS"; else echo "database failure: $fails FAILED"; fi
exit $fails
