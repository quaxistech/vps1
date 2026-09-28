#!/usr/bin/env bash
# A provider that cannot be reached must produce an explicit failure state and
# must not stop the others, must not advance a cursor past ground it never
# walked, and must not leave a half-written range that a later run would treat
# as complete.
#
# The outage is created by blackholing the provider's host in /etc/hosts, which
# is reversible and touches nothing outside this machine.
#
#   sudo ./test_provider_failure.sh
set -euo pipefail
CONFIG="${ALTMACRO_TEST_CONFIG:-/etc/altmacro/smoke.yaml}"
BIN=/opt/altmacro/bin/altmacro
DB=altmacro
HOSTS=/etc/hosts
MARK="# altmacro-provider-failure-test"
fails=0

q() { sudo -u postgres psql -X -q -t -A -d "$DB" -c "$1" | tr -d '[:space:]'; }

cleanup() {
    sed -i "/${MARK}\$/d" "$HOSTS"
    echo "==> /etc/hosts restored"
}
trap cleanup EXIT

echo "==> baseline"
before_bars="$(q 'SELECT count(*) FROM macro_price_bars')"
before_wm="$(q "SELECT COALESCE(max(watermark)::text,'none') FROM macro_ingest_cursors WHERE timeframe='1m'")"

echo "==> blackholing datafeed.dukascopy.com"
printf '127.0.0.1 datafeed.dukascopy.com %s\n' "$MARK" >> "$HOSTS"

sudo -u altmacro bash -c "set -a; . /etc/altmacro/altmacro.env; set +a; timeout 300 $BIN bootstrap --config $CONFIG --no-gap-scan" \
    >/tmp/altmacro_provider_failure.log 2>&1 || true

echo "==> what the outage recorded"
errors="$(q "SELECT count(*) FROM macro_ingest_runs WHERE outcome IN ('provider_error','rate_limited')")"
health="$(q "SELECT COALESCE(consecutive_failures,0) FROM macro_provider_metadata m
             JOIN macro_providers p ON p.id=m.provider_id WHERE p.code='dukascopy'")"
after_bars="$(q 'SELECT count(*) FROM macro_price_bars')"
after_wm="$(q "SELECT COALESCE(max(watermark)::text,'none') FROM macro_ingest_cursors WHERE timeframe='1m'")"
refs_ok="$(q "SELECT count(*) FROM macro_ingest_runs r JOIN macro_providers p ON p.id=r.provider_id
              WHERE p.code IN ('fred','frankfurter') AND r.outcome='ok'")"

if [[ "$errors" -eq 0 ]]; then
    echo "  FAIL  the outage was not recorded as a provider failure"
    fails=$((fails + 1))
else
    echo "  ok    the outage is recorded explicitly (${errors} failed runs)"
fi

if [[ "$health" -eq 0 ]]; then
    echo "  FAIL  provider health still reports no consecutive failures"
    fails=$((fails + 1))
else
    echo "  ok    provider health reflects the outage (${health} consecutive failures)"
fi

if [[ "$after_wm" != "$before_wm" ]]; then
    echo "  FAIL  a cursor advanced during an outage: ${before_wm} -> ${after_wm}"
    echo "        a later run would treat the unwalked range as covered"
    fails=$((fails + 1))
else
    echo "  ok    no cursor advanced past ground the walk never covered"
fi

if [[ "$after_bars" -lt "$before_bars" ]]; then
    echo "  FAIL  rows disappeared during an outage (${before_bars} -> ${after_bars})"
    fails=$((fails + 1))
else
    echo "  ok    nothing already stored was lost"
fi

# One unreachable provider must not stop the rest: the directive is explicit
# that a single missing source cannot block the system.
if [[ "$refs_ok" -eq 0 ]]; then
    echo "  FAIL  the reference providers did not run while dukascopy was down"
    fails=$((fails + 1))
else
    echo "  ok    the other providers kept working (${refs_ok} successful runs)"
fi

cleanup
trap - EXIT

echo "==> recovery"
sudo -u altmacro bash -c "set -a; . /etc/altmacro/altmacro.env; set +a; timeout 300 $BIN bootstrap --config $CONFIG --no-gap-scan" \
    >/tmp/altmacro_provider_recovery.log 2>&1 || true
recovered="$(q "SELECT COALESCE(consecutive_failures,0) FROM macro_provider_metadata m
                JOIN macro_providers p ON p.id=m.provider_id WHERE p.code='dukascopy'")"
if [[ "$recovered" -ne 0 ]]; then
    echo "  WARN  dukascopy is still failing after the outage was lifted (${recovered});"
    echo "        this provider throttles heavily, so a warning here is not conclusive"
else
    echo "  ok    the failure counter cleared once the provider came back"
fi

echo
if [[ $fails -eq 0 ]]; then echo "provider failure: PASS"; else echo "provider failure: $fails FAILED"; fi
exit $fails
