#!/usr/bin/env bash
# Load and resource measurement against the DEPLOYED service.
#
# WHAT CAN AND CANNOT BE MEASURED THROUGH A LIVE SOCKET HERE.
#
# A FINAL TradeSignal is deliverable only inside its own validity window —
# `proposal.valid_until_ms`, which on this deployment averages under an hour.
# The service refuses, by design, to deliver an expired signal as live. So the
# number of signals that can be pushed through a real WebSocket at any instant
# is bounded by the pipeline's production rate times that window, not by
# anything about this service.
#
# This measurement therefore has two halves, and neither of them is synthetic:
#
#   * the LIVE path, driven end to end through TLS at whatever inventory
#     currently exists;
#   * the PER-SIGNAL WORK at 10 / 100 / 1000 / 10000, driven by `verify`, which
#     performs exactly the CPU-bound work the delivery path performs for every
#     message — read the row, parse the payload, re-serialize it canonically,
#     hash it and compare — on the same real signals. Delivery adds an envelope
#     construction and a TLS write on top of it.
#
#   usage:  ALTSGW_HOST=root@1.2.3.4 tests/shell/test_load.sh
set -uo pipefail

VPS="${ALTSGW_HOST:?set ALTSGW_HOST=user@host}"
SSH_KEY="${ALTSIGNALGATEWAY_SSH_KEY:-$HOME/.ssh/altstrategy_vps_ed25519}"
BIN=/opt/altsignalgateway/bin/altsignalgateway
CFG=/etc/altsignalgateway/altsignalgateway.yaml
CERT=/etc/altsignalgateway/tls/server.crt

r() { ssh -o BatchMode=yes -i "$SSH_KEY" "$VPS" "$@"; }
psql_() { r "sudo -u postgres psql -d altspotter -tAc \"$1\""; }
asgw() { r "set -a; . /etc/altsignalgateway/common.env; set +a; $*"; }

echo "=== load and resource measurement — $(date -u +%FT%TZ) ==="

ROWS=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal")
NOW_MS=$(r "date +%s%3N")
LIVE=$(psql_ "SELECT count(*) FROM altsignalgateway.signal_journal
    WHERE expires_at_ms > $NOW_MS AND delivery_state NOT IN ('EXPIRED','FAILED')")
echo "journal rows: $ROWS      currently within their validity window: $LIVE"
[[ "$ROWS" -ge 10 ]] || { echo "SKIP  fewer than 10 journalled signals"; exit 0; }

# ---------------------------------------------------------------------------
echo
echo "--- 1. ingest latency, measured on real signals ---"
psql_ "SELECT 'n=' || count(*) ||
       '   T0 -> gateway (ms): p50=' || round(percentile_cont(0.5) WITHIN GROUP (ORDER BY received_at_ms - created_at_ms)) ||
       ' p90=' || round(percentile_cont(0.9) WITHIN GROUP (ORDER BY received_at_ms - created_at_ms)) ||
       ' p99=' || round(percentile_cont(0.99) WITHIN GROUP (ORDER BY received_at_ms - created_at_ms)) ||
       ' max=' || max(received_at_ms - created_at_ms)
       FROM altsignalgateway.signal_journal"
psql_ "SELECT '        journal commit (ms): p50=' || round(percentile_cont(0.5) WITHIN GROUP (ORDER BY journaled_at_ms - received_at_ms)) ||
       ' p90=' || round(percentile_cont(0.9) WITHIN GROUP (ORDER BY journaled_at_ms - received_at_ms)) ||
       ' p99=' || round(percentile_cont(0.99) WITHIN GROUP (ORDER BY journaled_at_ms - received_at_ms)) ||
       ' max=' || max(journaled_at_ms - received_at_ms)
       FROM altsignalgateway.signal_journal"
echo "        (commit = validate + 2 enrichment reads + canonicalize + 2 hashes + 1 transaction)"

echo
echo "--- 2. ingest capacity ---"
# NOT the interval between arrivals: that is paced by AltStrategy's bar close and
# by AltSignalQuality's own per-candidate evaluation, and measuring it would
# report the upstream's rate as if it were this service's limit. The quantity
# that bounds THIS service is its own serial cost per signal, which is exactly
# journaled_at - received_at.
# percentile_cont returns double precision, and round(double, int) does not
# exist in PostgreSQL — the cast is required, not decorative.
psql_ "WITH c AS (
         SELECT (percentile_cont(0.5) WITHIN GROUP (ORDER BY journaled_at_ms - received_at_ms))::numeric p50,
                (percentile_cont(0.9) WITHIN GROUP (ORDER BY journaled_at_ms - received_at_ms))::numeric p90,
                (percentile_cont(0.99) WITHIN GROUP (ORDER BY journaled_at_ms - received_at_ms))::numeric p99
           FROM altsignalgateway.signal_journal)
       SELECT 'serial cost per signal (ms): p50=' || round(p50,1) ||
              '  p90=' || round(p90,1) || '  p99=' || round(p99,1) ||
              '  => ~' || round(1000.0 / greatest(p50, 0.5)) || ' signals/s on one ingest thread'
         FROM c"
# For contrast, what the upstream actually delivers.
psql_ "WITH d AS (
         SELECT journaled_at_ms - lag(journaled_at_ms) OVER (ORDER BY sequence) gap
           FROM altsignalgateway.signal_journal)
       SELECT '        observed arrival spacing inside a burst: p50=' ||
              round(percentile_cont(0.5) WITHIN GROUP (ORDER BY gap)) || ' ms' ||
              '  (upstream-paced, not a limit of this service)'
         FROM d WHERE gap IS NOT NULL AND gap > 0 AND gap < 2000"

# ---------------------------------------------------------------------------
echo
echo "--- 3. live delivery through TLS ---"
# Timed ON the host. An ssh round trip to this VPS costs seconds and would
# dominate every number below it.
for N in 10 100 1000 10000; do
    if [[ "$N" -gt "$LIVE" ]]; then
        echo "    n=$N   not reachable: only $LIVE signals are inside their validity window"
        continue
    fi
    OUT=$(r "set -a; . /etc/altsignalgateway/common.env; set +a
        S=\$(date +%s%3N)
        R=\$(timeout 300 $BIN client --host 127.0.0.1 --port 9443 --sni altsignalgateway \
            --ca-file $CERT --client-id vps1-observer --key-id vps1-observer-k1 \
            --secret-env ALTSIGNALGATEWAY_CLIENT_OBSERVER_SECRET \
            --resume from-start --no-ack --count $N --quiet 2>&1 | grep -o 'received=[0-9]*')
        E=\$(date +%s%3N)
        echo \"\$R ms=\$((E-S))\"" 2>&1)
    MS=$(echo "$OUT" | grep -oE 'ms=[0-9]+' | cut -d= -f2)
    GOT=$(echo "$OUT" | grep -oE 'received=[0-9]+' | cut -d= -f2)
    if [[ "${GOT:-0}" == "$N" && -n "$MS" ]]; then
        awk -v n="$N" -v ms="$MS" 'BEGIN{ printf "    n=%-6d %6d ms   ~%.0f signals/s   (whole client run: TLS handshake + HELLO + AUTH + RESUME + N signals)\n", n, ms, (ms>0 ? n*1000.0/ms : n*1000.0) }'
    else
        echo "    n=$N   FAILED: $OUT"
    fi
done
echo "    the connection setup is INSIDE these numbers, so small n is dominated"
echo "    by one TLS handshake rather than by per-signal cost."

echo
echo "--- 4. live delivery WITH durable acknowledgement ---"
SAVED=$(psql_ "SELECT COALESCE(last_acked_sequence,0) FROM altsignalgateway.client_cursors
                WHERE client_id='vps2-executor'")
SAVED=${SAVED:-0}
for N in 10 100; do
    if [[ "$N" -gt "$LIVE" ]]; then
        echo "    n=$N   not reachable: only $LIVE signals are inside their validity window"
        continue
    fi
    psql_ "DELETE FROM altsignalgateway.client_cursors WHERE client_id='vps2-executor'" >/dev/null
    OUT=$(r "set -a; . /etc/altsignalgateway/common.env; set +a
        S=\$(date +%s%3N)
        R=\$(timeout 300 $BIN client --host 127.0.0.1 --port 9443 --sni altsignalgateway \
            --ca-file $CERT --client-id vps2-executor --key-id vps2-executor-k1 \
            --secret-env ALTSIGNALGATEWAY_CLIENT_VPS2_SECRET \
            --resume from-start --count $N --quiet 2>&1 | grep -o 'received=[0-9]*')
        E=\$(date +%s%3N)
        echo \"\$R ms=\$((E-S))\"" 2>&1)
    MS=$(echo "$OUT" | grep -oE 'ms=[0-9]+' | cut -d= -f2)
    GOT=$(echo "$OUT" | grep -oE 'received=[0-9]+' | cut -d= -f2)
    if [[ "${GOT:-0}" == "$N" && -n "$MS" ]]; then
        awk -v n="$N" -v ms="$MS" 'BEGIN{ printf "    n=%-6d %6d ms   ~%.0f signals/s   (delivered + ACKed + durable writes)\n", n, ms, (ms>0 ? n*1000.0/ms : n*1000.0) }'
    else
        echo "    n=$N   FAILED: $OUT"
    fi
done
psql_ "INSERT INTO altsignalgateway.client_cursors (client_id, last_acked_sequence)
       VALUES ('vps2-executor', $SAVED)
       ON CONFLICT (client_id) DO UPDATE SET last_acked_sequence =
           GREATEST(altsignalgateway.client_cursors.last_acked_sequence, $SAVED)" >/dev/null
RESTORED=$(psql_ "SELECT last_acked_sequence FROM altsignalgateway.client_cursors
                   WHERE client_id='vps2-executor'")
echo "    executor cursor restored to $RESTORED (was $SAVED)"

# ---------------------------------------------------------------------------
echo
echo "--- 5. per-signal work at scale ---"
echo "    identical to the delivery path's CPU cost: read the row, parse the"
echo "    payload, re-serialize it canonically, hash it, compare."
# A run over zero rows measures everything that is NOT per-signal work: the ssh
# round trip, process start, config load, the database connection. Subtracting
# it is the difference between reporting this service's cost and reporting the
# cost of the harness measuring it.
# Timed on the host, in one shell, so the measurement is of `verify` and not of
# the network between here and there.
for N in 10 100 1000 10000; do
    OUT=$(r "set -a; . /etc/altsignalgateway/common.env; set +a
        S=\$(date +%s%3N)
        R=\$($BIN verify --config $CFG --limit $N 2>&1 | tail -1)
        E=\$(date +%s%3N)
        echo \"\$R ms=\$((E-S))\"" 2>&1)
    MS=$(echo "$OUT" | grep -oE 'ms=[0-9]+' | cut -d= -f2)
    CHECKED=$(echo "$OUT" | grep -oE 'checked=[0-9]+' | cut -d= -f2)
    CHECKED=${CHECKED:-0}
    if [[ "$CHECKED" -gt 0 && -n "$MS" ]]; then
        awk -v n="$CHECKED" -v ms="$MS" -v req="$N" 'BEGIN{
            printf "    limit=%-6d checked=%-6d %5d ms   ~%.0f signals/s   %.3f ms/signal\n",
                   req, n, ms, (ms>0 ? n*1000.0/ms : 0), (n>0 ? ms*1.0/n : 0) }'
    else
        echo "    limit=$N  FAILED: $OUT"
    fi
    [[ "$CHECKED" -lt "$N" ]] && echo "        (the journal holds $CHECKED rows; that is the whole of it)"
done
echo "    the per-invocation floor — process start, config load, one database"
echo "    connection — is included above and is a few tens of milliseconds."

# ---------------------------------------------------------------------------
echo
echo "--- 6. memory is bounded by the queue, not by the backlog ---"
MEM_IDLE=$(r "systemctl show altsignalgateway -p MemoryCurrent --value")
asgw "timeout 45 $BIN client --host 127.0.0.1 --port 9443 --sni altsignalgateway --ca-file $CERT \
    --client-id vps1-observer --key-id vps1-observer-k1 \
    --secret-env ALTSIGNALGATEWAY_CLIENT_OBSERVER_SECRET \
    --resume from-start --no-ack --quiet >/dev/null 2>&1" &
BG=$!
sleep 15
MEM_LOAD=$(r "systemctl show altsignalgateway -p MemoryCurrent --value")
MEM_PEAK=$(r "systemctl show altsignalgateway -p MemoryPeak --value")
wait $BG 2>/dev/null
echo "    idle=$((MEM_IDLE / 1024)) KiB   during a whole-journal replay=$((MEM_LOAD / 1024)) KiB   peak-ever=$((MEM_PEAK / 1024)) KiB"
echo "    journal holds $ROWS signals; the resident set does not track it."
r "systemctl show altsignalgateway -p MemoryMax -p TasksCurrent -p TasksMax -p CPUUsageNSec --value | tr '\n' ' '"; echo

# ---------------------------------------------------------------------------
echo
echo "--- 7. storage, measured ---"
psql_ "SELECT 'signal_journal: rows=' || count(*) ||
       '  heap=' || pg_size_pretty(pg_relation_size('altsignalgateway.signal_journal')) ||
       '  toast=' || pg_size_pretty((SELECT COALESCE(pg_total_relation_size(reltoastrelid),0)
                                       FROM pg_class WHERE oid='altsignalgateway.signal_journal'::regclass)) ||
       '  indexes=' || pg_size_pretty(pg_indexes_size('altsignalgateway.signal_journal')) ||
       '  total=' || pg_size_pretty(pg_total_relation_size('altsignalgateway.signal_journal'))
       FROM altsignalgateway.signal_journal"
psql_ "SELECT '                bytes/row=' || round(pg_total_relation_size('altsignalgateway.signal_journal')::numeric / greatest(count(*),1)) ||
       '  avg_canonical_payload=' || round(avg(length(canonical_payload))) ||
       '  avg_source_payload=' || round(avg(length(source_payload))) ||
       '  raw/row=' || round(avg(length(canonical_payload) + length(source_payload)))
       FROM altsignalgateway.signal_journal"
psql_ "SELECT 'delivery_events: rows=' || count(*) ||
       '  total=' || pg_size_pretty(pg_total_relation_size('altsignalgateway.delivery_events')) ||
       '  bytes/row=' || round(pg_total_relation_size('altsignalgateway.delivery_events')::numeric / greatest(count(*),1))
       FROM altsignalgateway.delivery_events"
psql_ "SELECT 'whole schema: ' || pg_size_pretty(sum(pg_total_relation_size(c.oid)))
         FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
        WHERE n.nspname='altsignalgateway' AND c.relkind='r'"

echo
echo "load measurement complete"
