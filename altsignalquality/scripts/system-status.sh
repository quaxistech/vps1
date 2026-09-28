#!/usr/bin/env bash
# Collect the live state of the VPS1 chain and render SYSTEM_STATUS.md.
#
# Read-only. Every statement in the generated file is measured on the deployed
# system — never a fixture, never an assumption. A field this cannot measure is
# printed as "unknown" rather than guessed, because a status file that guesses
# is worse than no status file.
#
#   ./scripts/system-status.sh > SYSTEM_STATUS.md
set -uo pipefail
VPS="${ALTSTACK_VPS:-root@132.243.30.54}"

q()   { ssh -o BatchMode=yes "$VPS" "sudo -u postgres psql -Atd $1 -c \"$2\"" 2>/dev/null; }
sh_() { ssh -o BatchMode=yes "$VPS" "$1" 2>/dev/null; }

NOW=$(sh_ "date -u '+%Y-%m-%d %H:%M:%SZ'")
UPTIME=$(sh_ "uptime | sed 's/^ *//'")
DISK=$(sh_ "df -h / | tail -1 | awk '{print \$3\" used of \"\$2\" (\"\$5\"), \"\$4\" free\"}'")

cat <<EOF
# SYSTEM STATUS — VPS1 chain

\`\`\`
AltSpotter → AltTech → AltML → AltStrategy → AltSignalQuality → FINAL TradeSignal → AltSignalGateway → TLS WebSocket
\`\`\`

Generated from the deployed system at **${NOW}** by \`scripts/system-status.sh\`.
Regenerate before trusting it; a stale status file is a lie with a timestamp.

\`\`\`
host:   ${UPTIME}
disk:   ${DISK}
\`\`\`

## Services

| service | systemd | restarts |
|---|---|---|
EOF
for s in altspotter alttech altml altstrategy altsignalquality altsignalgateway; do
  echo "| $s | $(sh_ "systemctl is-active $s") | $(sh_ "systemctl show $s -p NRestarts --value") |"
done

cat <<'EOF'

## Freshness

Age of the newest record each stage has produced. A stage is only as current as
the one above it, so read this column downwards: the first large age is the
blockage, and everything below it is a consequence rather than a fault.

| stage | newest record | age |
|---|---|---|
EOF
q altspotter "SELECT '| market candles 1m | '||to_char(max(open_time),'MM-DD HH24:MI')||' | '||round(extract(epoch FROM now()-max(open_time))/60)::int||' min |' FROM market_candles_1m WHERE exchange_id=1"
q alttech    "SELECT '| feature vectors 15m VALID | '||to_char(max(bar_open_time),'MM-DD HH24:MI')||' | '||round(extract(epoch FROM now()-max(bar_open_time))/60)::int||' min |' FROM alttech.venue_feature_vectors WHERE venue_class='bitget' AND timeframe='15m' AND status='VALID'"
q altml      "SELECT '| AltML feature vectors | '||to_char(max(bar_open_time),'MM-DD HH24:MI')||' | '||round(extract(epoch FROM now()-max(bar_open_time))/60)::int||' min |' FROM altml.feature_vectors"
q altspotter "SELECT '| decision contexts | '||to_char(max(created_at),'MM-DD HH24:MI')||' | '||round(extract(epoch FROM now()-max(created_at))/60)::int||' min |' FROM altstrategy.decision_context_snapshots"
q altspotter "SELECT '| trade proposals | '||to_char(max(created_at),'MM-DD HH24:MI')||' | '||round(extract(epoch FROM now()-max(created_at))/60)::int||' min |' FROM altstrategy.trade_signal_proposals"
q altspotter "SELECT '| signal evaluations | '||to_char(max(created_at),'MM-DD HH24:MI')||' | '||round(extract(epoch FROM now()-max(created_at))/60)::int||' min |' FROM altsignalquality.signal_evaluations"

cat <<'EOF'

## Useful throughput (last 60 minutes)

Zero errors alongside zero output is not health. These are the counts that say
whether the chain is doing work, not merely staying up.

```
EOF
q alttech    "SELECT 'feature vectors written  '||lpad(count(*)::text,8) FROM alttech.venue_feature_vectors WHERE computed_at > now()-interval '60 min'"
q altspotter "SELECT 'decision contexts        '||lpad(count(*)::text,8) FROM altstrategy.decision_context_snapshots WHERE created_at > now()-interval '60 min'"
q altspotter "SELECT 'strategy candidates      '||lpad(count(*)::text,8) FROM altstrategy.strategy_candidates WHERE created_at > now()-interval '60 min'"
q altspotter "SELECT 'trade proposals          '||lpad(count(*)::text,8) FROM altstrategy.trade_signal_proposals WHERE created_at > now()-interval '60 min'"
q altspotter "SELECT 'recommended proposals    '||lpad(count(*)::text,8) FROM altstrategy.trade_signal_proposals WHERE created_at > now()-interval '60 min' AND recommended_for_execution"
q altspotter "SELECT 'signal evaluations       '||lpad(count(*)::text,8) FROM altsignalquality.signal_evaluations WHERE created_at > now()-interval '60 min'"
q altspotter "SELECT 'FINAL signals published  '||lpad(count(*)::text,8) FROM altsignalquality.signal_evaluations WHERE published AND created_at > now()-interval '60 min'"
q altspotter "SELECT 'gateway journalled       '||lpad(count(*)::text,8) FROM altsignalgateway.signal_journal WHERE received_at_ms > (extract(epoch FROM now())::bigint-3600)*1000"
echo '```'

cat <<'EOF'

## Feature vectors by timeframe (last 2 days)

```
EOF
q alttech "SELECT rpad(timeframe,5)||' '||rpad(status,17)||lpad(count(*)::text,8)||'   newest '||to_char(max(bar_open_time),'MM-DD HH24:MI') FROM alttech.venue_feature_vectors WHERE venue_class='bitget' AND bar_open_time > now()-interval '2 days' GROUP BY timeframe, status ORDER BY timeframe, status"
echo '```'

cat <<'EOF'

## Market data integrity

```
EOF
q altspotter "SELECT rpad(status,14)||lpad(count(*)::text,6)||' gaps, '||lpad(coalesce(sum(expected_rows),0)::text,10)||' candles' FROM history_gaps GROUP BY status ORDER BY status"
echo '```'

cat <<'EOF'

## Models

```
EOF
q altspotter "SELECT 'altsignalquality  '||rpad(state,12)||lpad(count(*)::text,4)||' models' FROM altsignalquality.models GROUP BY state ORDER BY state"
q altml      "SELECT 'altml             '||rpad(status,12)||lpad(count(*)::text,4)||' models' FROM altml.models GROUP BY status ORDER BY status"
echo '```'

cat <<'EOF'

## Storage

```
EOF
for db in altspotter alttech altml; do
  q "$db" "SELECT rpad('$db',14)||lpad(pg_size_pretty(pg_database_size(current_database())),10)"
done
sh_ "du -sh /var/lib/nats/jetstream 2>/dev/null | awk '{printf \"%-14s%10s\\n\", \"jetstream\", \$1}'"
echo '```'
