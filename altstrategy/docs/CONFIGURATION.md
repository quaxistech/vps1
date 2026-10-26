# Configuration

One production config: `/etc/altstrategy/altstrategy.yaml`
(`config/altstrategy.example.yaml`); shared infra in
`/etc/altstrategy/common.yaml` (NATS URL, PostgreSQL DSN). No per-strategy config
files — strategy parameter sets are versioned in the database.

## Sections

- `postgres_pool_size` — shared PG pool size.
- `inputs` — `alttech_subjects`, `prediction_subjects`, `universe_subjects`.
- `context` — `context_ttl_sec`, `max_component_age_sec`, and the ordered
  `bitget_feature_names` / `cross_venue_feature_names` that decode AltTech's
  compact vectors (must match the AltTech feature-set order).
- `strategies` — `min_candidate_rr`, `candidate_ttl_sec`.
- `adaptive_allocator` — `prior_strength` (Bayesian k), `edge_scale`,
  `weight_min/max`, `half_life_days`.
- `ranking` — not configurable: the policies (`champion_ml_v2`,
  `challenger_static_v1`) and the close gap are compiled in.
- `trade_plan` — `min_stop_bps`, `max_entry_timeout_sec`,
  `default_max_slippage_bps`, `max_holding_hours` (48).
- `outcomes` — `bitget_exchange_id`, `candles_table`, `trades_table`,
  `prefer_raw_trades`, `scan_interval_sec`, and the price-grid tables
  `instrument_specs_table` / `pair_map_table`.
- `publication` — the outbox publisher's `ack_timeout_ms` and `max_age_hours`
  (dead-letter age).
- `learning` — `min_plan_geometry_version` (1 or 2; anything else refuses to
  start): the oldest plan geometry whose outcomes posteriors, drift, lifecycle
  and calibration may learn from.
- `execution_feedback` — subject, `required: false`.
- `drift` / `calibration` / `lifecycle` — intervals, min samples, cooldown;
  `calibration.enabled` switches the parameter calibrator.
- `training_snapshots` — `export_root`, `format` (csv|parquet), `interval_sec`,
  retention windows.
- `resources` — `max_inflight_pairs`, `batch_db_writes`.
- `logging` — `level`.

## Validation

`altstrategy-config-check --file <yaml> --kind <common|service>` validates the
config before start. The deploy runs the common-config check against the new
release binary. `${ENV_VAR}` placeholders are expanded from the environment;
a missing variable is an error (secrets stay explicit).

`trade_plan.max_holding_hours` accepts an integer from 1 through 48; absence
retains the default 48. Supplied malformed, negative, zero, oversized and
out-of-int64 values are startup errors. Validation precedes hours-to-ms
multiplication. Direct PlanConfig callers independently require holding caps
between five minutes (the existing validity floor) and 48 hours. Valid holding
classes, expiry formulas and geometry multipliers are unchanged.
