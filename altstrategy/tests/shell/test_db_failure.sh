#!/usr/bin/env bash
# Database failure during runtime, on a SCRATCH PostgreSQL cluster -- never the
# production database. Requires PostgreSQL 16 server binaries (PGBIN), a
# nats-server on PATH, and a built altstrategy-service (BIN). Uses systemd
# --user transient units so every process is capped and cleaned up.
#
#   PGBIN=~/.local/pgsql16/usr/lib/postgresql/16/bin \
#   BIN=build/release/bin/altstrategy-service tests/shell/test_db_failure.sh
#
# Asserts:
#   1. the service survives a kill -9 of the postmaster: stays active, no busy
#      loop, database errors logged as errors;
#   2. after the database returns it re-acquires its singleton lock
#      (altstrategy_singleton_lock_reacquired, one advisory lock held);
#   3. a second instance is then refused (altstrategy_instance_already_running);
#   4. if another instance takes the lock first, this one stops with exit 1
#      (altstrategy_singleton_lock_lost) -- two engines never run together;
#   5. SIGTERM stops it within the stop budget.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PGBIN="${PGBIN:?set PGBIN to the PostgreSQL 16 bin directory}"
BIN="${BIN:-$ROOT/build/release/bin/altstrategy-service}"
W="$(mktemp -d)"
PGPORT="${PGPORT:-55432}"
NATSPORT="${NATSPORT:-14224}"
TAG="as-dbfail-$$"
cleanup() {
    for u in "$TAG-a" "$TAG-b" "$TAG-pg" "$TAG-nats"; do
        systemctl --user stop "$u" 2>/dev/null || true
        systemctl --user reset-failed "$u" 2>/dev/null || true
    done
    rm -rf "$W"
}
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
psqlc() { "$PGBIN/psql" -h 127.0.0.1 -p "$PGPORT" -U postgres "$@"; }
start_pg() {
    systemctl --user reset-failed "$TAG-pg" 2>/dev/null || true
    systemd-run --user --quiet --unit="$TAG-pg" -p MemoryMax=512M "$PGBIN/postgres" -D "$W/data" \
        -p "$PGPORT" -k "$W" -c listen_addresses=127.0.0.1 -c fsync=off
    timeout 60 bash -c "until '$PGBIN/pg_isready' -h 127.0.0.1 -p $PGPORT >/dev/null; do sleep 0.5; done"
}
start_as() {   # $1 = unit suffix
    systemctl --user reset-failed "$TAG-$1" 2>/dev/null || true
    systemd-run --user --quiet --unit="$TAG-$1" -p MemoryMax=600M -p EnvironmentFile="$W/env" \
        "$BIN" --config "$W/altstrategy.yaml" --common "$W/common.yaml"
}
logs() { journalctl --user -u "$TAG-$1" -o cat 2>/dev/null; }
locks() { psqlc -d altstrategy -At -c "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND granted"; }

"$PGBIN/initdb" -D "$W/data" -A trust -U postgres --no-sync >/dev/null
start_pg
psqlc -q -c "CREATE ROLE altstrategy_app LOGIN" -c "CREATE DATABASE altstrategy OWNER altstrategy_app"
# The real migrator, as a release is migrated: the service refuses a schema
# whose ledger has not reached the version it writes.
"$(dirname "$BIN")/altstrategy-db-migrate" \
    --dsn "host=127.0.0.1 port=$PGPORT dbname=altstrategy user=altstrategy_app" \
    --migrations "$ROOT/db/migrations" >/dev/null
mkdir -p "$W/js" "$W/exports"
systemd-run --user --quiet --unit="$TAG-nats" -p MemoryMax=256M nats-server -a 127.0.0.1 -p "$NATSPORT" -js -sd "$W/js"
cp "$ROOT/config/common.example.yaml" "$W/common.yaml"
sed "s#export_root: .*#export_root: $W/exports#" "$ROOT/config/altstrategy.example.yaml" > "$W/altstrategy.yaml"
cp "$ROOT/config/symbols.yaml" "$W/"
cat > "$W/env" <<EOF
ALTSTRATEGY_ENVIRONMENT=test
ALTSTRATEGY_LOG_LEVEL=info
ALTSTRATEGY_INSTANCE_ID=$TAG
ALTSTRATEGY_NATS_URL=nats://127.0.0.1:$NATSPORT
ALTSTRATEGY_POSTGRES_DSN=host=127.0.0.1 port=$PGPORT dbname=altstrategy user=altstrategy_app connect_timeout=3
EOF

start_as a
timeout 60 bash -c "until journalctl --user -u $TAG-a -o cat | grep -q service_started; do sleep 1; done" \
    || fail "service did not start"
[[ "$(locks)" == 1 ]] || fail "singleton lock not held after start"
# Every startup write must succeed against the real schema: the catalogue
# upsert once passed 7 arguments to an 8-placeholder statement and failed on
# every start (2026-09-26), which no unit test executes.
[[ "$(psqlc -d altstrategy -At -c 'SELECT count(*) FROM altstrategy.strategy_definitions')" == 16 ]] \
    || fail "strategy catalogue not written at startup"
! logs a | grep -q db_write_failed || fail "a startup write failed: $(logs a | grep db_write_failed | head -1)"

# 1. database dies under the running service
systemctl --user kill -s SIGKILL "$TAG-pg"
sleep 45
systemctl --user is-active --quiet "$TAG-a" || fail "service died with the database"
pid=$(systemctl --user show "$TAG-a" -p MainPID --value)
cpu=$(ps -o pcpu= -p "$pid" | tr -d ' ')
awk "BEGIN{exit !($cpu < 20)}" || fail "busy loop while the database is down (cpu $cpu%)"

# 2. database returns: the lock is re-acquired
start_pg
timeout 90 bash -c "until journalctl --user -u $TAG-a -o cat | grep -q altstrategy_singleton_lock_reacquired; do sleep 2; done" \
    || fail "singleton lock not re-acquired after the database returned"
[[ "$(locks)" == 1 ]] || fail "expected exactly one advisory lock"

# 3. a second instance is refused
start_as b
timeout 30 bash -c "until journalctl --user -u $TAG-b -o cat | grep -q altstrategy_instance_already_running; do sleep 1; done" \
    || fail "a second instance was not refused"

# 4. another instance takes the lock first: this one stops, exit 1
systemctl --user kill -s SIGKILL "$TAG-pg"
sleep 3
start_pg
start_as b
timeout 30 bash -c "until journalctl --user -u $TAG-b -o cat | grep -q service_started; do sleep 1; done" \
    || fail "instance b did not start on a free lock"
timeout 90 bash -c "while systemctl --user is-active --quiet $TAG-a; do sleep 2; done" \
    || fail "instance a kept running without its lock"
[[ "$(systemctl --user show "$TAG-a" -p ExecMainStatus --value)" == 1 ]] || fail "instance a did not exit 1"
logs a | grep -q altstrategy_singleton_lock_lost || fail "lock loss not reported"
[[ "$(locks)" == 1 ]] || fail "expected exactly one advisory lock"

# 5. SIGTERM
t0=$(date +%s%N)
systemctl --user stop "$TAG-b"
dt=$(( ($(date +%s%N) - t0) / 1000000 ))
(( dt < 25000 )) || fail "SIGTERM stop took ${dt} ms"
echo "PASS: db failure / recovery / singleton (stop ${dt} ms)"
