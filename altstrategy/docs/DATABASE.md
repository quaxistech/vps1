# Database

AltStrategy owns **only** the `altstrategy` PostgreSQL schema
(`db/migrations/0001_altstrategy_schema.sql`, 40 tables). It never creates or
modifies the AltSpotter / AltTech / AltML schemas; it reads AltSpotter market
history read-only. Migrations are applied by `altstrategy-db-migrate` (ordered,
checksummed, advisory-locked) — never automatically by the service or by deploy.

The `altstrategy` schema is deployed **inside the shared AltSpotter database**
(not a separate database): the service uses a single PostgreSQL connection pool
for both its own schema and the read-only `market_candles_1m` /
`market_trades_raw` tables the market-outcome tracker reads, so both must be
reachable from the same DSN. Its role (`altstrategy_app` by convention) has:
full rights on its own `altstrategy` schema, `SELECT` on the two AltSpotter
tables above, and `search_path = altstrategy, public` so unqualified table
names resolve correctly on both sides.

## Table groups

- **Catalogue / versions / parameters / lifecycle**: `strategy_definitions`,
  `strategy_versions`, `strategy_parameter_schemas`, `strategy_parameter_sets`,
  `strategy_lifecycle`.
- **Tier 1 — candidates**: `strategy_candidates`, `strategy_candidate_reasons`,
  `strategy_ensemble_snapshots`.
- **Ranking**: `ranking_policies`, `ranking_policy_versions`,
  `strategy_rankings`, `ranking_components`.
- **Plans / proposals**: `trade_plan_variants`, `trade_signal_proposals`
  (with hard price-side + ≤48h CHECK constraints), `proposal_warnings`,
  `proposal_state_events`.
- **Point-in-time context**: `decision_context_snapshots`.
- **Learning loop**: `market_outcomes`, `execution_feedback`,
  `counterfactual_outcomes`, `outcome_attribution`.
- **Adaptive stats**: `strategy_stats_lifetime`, `strategy_stats_rolling`,
  `strategy_stats_decayed`, `strategy_posteriors`.
- **Adaptation runs**: `strategy_drift_snapshots`,
  `parameter_calibration_runs`, `ranking_challenger_runs`, `lifecycle_events`.
- **Training archive**: `signal_training_examples`, `signal_training_labels`,
  `signal_training_context`.
- **Snapshot bookkeeping**: `signal_archive_snapshots`, `signal_archive_files`,
  `signal_archive_exports`.
- **Ops**: `computation_runs`, `validation_results`, `retention_policies`,
  `retention_runs`.

## Idempotency

Every entity carries an immutable id that is **deterministic in its natural
key** (`ids::make(prefix, natural_key)`), plus a `UNIQUE(natural_key)` (or
composite unique) constraint. All writes are `ON CONFLICT DO NOTHING` /
`DO UPDATE`, so NATS redelivery or a restart replaying a producer step never
duplicates a row.

## Storage and retention

The schema contains per-table retention policies, enforced by the maintenance
scheduler. `altstrategy.retention_policies` is the source of truth; database
retention and immutable snapshot-file retention are described in
[Snapshot and archive retention](SNAPSHOT_RETENTION.md).

## Price-side invariant at the storage boundary

`trade_signal_proposals` enforces LONG `TP>entry AND SL<entry`, SHORT
`TP<entry AND SL>entry`, and `max_holding_time_ms <= 172800000` (48 h) as CHECK
constraints — a second line of defence behind the proposal builder's INVALID
gate.
