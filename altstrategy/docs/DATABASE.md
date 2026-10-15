# Database

AltStrategy owns **only** the `altstrategy` PostgreSQL schema
(`db/migrations/0001_altstrategy_schema.sql`, 40 tables). It never creates or
modifies the AltSpotter / AltTech / AltML schemas; it reads AltSpotter market
history read-only. Migrations are applied by `altstrategy-db-migrate` (ordered,
checksummed, advisory-locked) — never automatically by the service or by deploy.

In production (read-only inventory, 2026-10-15) every VPS1 service has its own
database. The `altstrategy` database holds this schema and the Gateway's
`altsignalgateway` schema. AltSpotter's tables reach it as `postgres_fdw`
foreign tables on the server `altspotter_srv`, owned by `altstrategy_app` and
read through the one connection pool with `search_path = altstrategy, public`:

| foreign table | read by | defined in |
|---|---|---|
| `public.market_candles_1m`, `public.market_trades_raw` | the market-outcome path reader | [`db/fdw/altspotter_reference.sql`](../db/fdw/altspotter_reference.sql) (recorded from the production catalog; they predate the file) |
| `public.instrument_spec_versions`, `public.canonical_pair_exchange_map` | the price grid at T0 (plan geometry 2) | the same file; applied by an operator, with approval |

The remote role has no write privilege on any of them. The `altspotter_ref` and
`alttech_ref` foreign schemas in the same database belong to the Gateway's role.
AltSignalQuality reads six AltStrategy tables from its own database, through
its foreign schema `altstrategy` (server `altstrategy_srv`).

Operator SQL that is not a migration lives in `db/fdw/` and `db/operator/`
(`quality05_parameter_reset.sql`: the calibrated parameter sets back to the
compiled defaults; `outbox_requeue_dead_lettered.sql`: dead-lettered messages
sent again); the migrator never runs either.

`altstrategy-db-migrate` records each applied file in `schema_migrations`
(unqualified, so `altstrategy.schema_migrations` under the database's
`search_path = altstrategy, public`; production holds 0001..0010 on
2026-10-15). Each file runs in its own transaction. The service refuses to
start (`schema_not_migrated`, exit 1) while that ledger is older than
`kRequiredSchemaMigration` (`decision_store.hpp`, the newest file in
`db/migrations/`): on an older schema every archive would fail and nothing
would be published.

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
- **Publication** (0011): `publication_outbox` -- every proposal and market
  outcome message, written in the transaction that archives it, sent by the
  outbox publisher ([NATS.md](NATS.md)). `settled_at` is set only when the
  stream acknowledges a row, and retention keys on it: neither a message still
  owed nor a dead-lettered one is ever reclaimed.

## Versions on the rows (0012, 0013)

| column | table | meaning |
|---|---|---|
| `plan_geometry_version` | `trade_plan_variants`, `trade_signal_proposals` | 1 (NULL): the target rounded onto the entry from the long side's excursion (QUALITY-01/02); 2: side-aware bps magnitudes on the venue's tick grid |
| `tick_size`, `tick_source` | `trade_plan_variants` | the grid the prices were rounded to: `instrument_spec` (the spec in force at T0), `config` or `derived` |
| `evaluator_version`, `fill_model`, `resolution` | `market_outcomes` | 1 (NULL): the original evaluator; 2: `conservative-1` ([MARKET_OUTCOMES.md](MARKET_OUTCOMES.md)) and the rule that decided |
| `path_bar_ms`, `path_gap_count` | `market_outcomes` | the path resolution and the gaps it had |
| `evidence_version` | `strategy_posteriors` | the minimum plan geometry the posterior was estimated from |

Outcomes of evaluator 2 store NULL returns for everything that was not a held
trade; learning reads only `TARGET_FIRST`, `STOP_FIRST` and `TIME_STOP`.

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
