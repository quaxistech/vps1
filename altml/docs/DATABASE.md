# Database

AltML owns exactly one PostgreSQL database, schema `altml`, initialized from
[../db/schema.sql](../db/schema.sql). Prospective extensions under
`db/migrations/` use the separate additive migration runner and ledger; they do
not rewrite that historical schema fingerprint. AltML never creates, alters or
drops anything in AltSpotter's or AltTech's databases.

## Read-only inputs, through `postgres_fdw`

`scripts/db-bootstrap.sh` creates a read-only reader role in each upstream
database and wires `postgres_fdw` foreign tables inside `altml`'s own
database, so the existing SQL joins in `label_builder`/`trainer` work exactly
as if the data were local:

| Foreign table (in `altml`) | Points at | Owner |
|---|---|---|
| `altml.fdw_candles_1m` | `public.market_candles_1m` | AltSpotter |
| `altml.fdw_exchange_instruments` | `public.exchange_instruments` | AltSpotter |
| `altml.fdw_exchanges` | `public.exchanges` | AltSpotter |
| `altml.fdw_feature_set_versions` | `alttech.feature_set_versions` | AltTech |
| `altml.fdw_feature_set_members` | `alttech.feature_set_members` | AltTech |

Only the columns AltML's queries actually use are exposed. No foreign key
crosses into another service's table — every reference is by value
(`exchange_id`, `instrument_id`, `schema_hash`).

## Why AltML keeps a local feature archive

AltTech retains `venue_feature_vectors` for only a few days (sized for live
order attribution, not model training), so it is **not** read through FDW for
training. Instead, the inference component archives every valid vector it
consumes from the live NATS stream into `altml.feature_vectors` as it
arrives — the trainer then joins that local table against `altml.labels`,
both owned by AltML, both retained long enough for the rolling training
window.

`original_live_payload` stores the first original live envelope independently
of the existing vector values. It can therefore preserve a live observation
even if backfill already occupied the archive key. NULL on legacy rows means
the original live bytes are unavailable; no historical reconstruction is
performed. See [Operations](OPERATIONS.md) for migration, capacity and recovery
requirements.

## Tables AltML owns

| Table | Purpose |
|---|---|
| `fee_models` | versioned round-trip fee/slippage assumptions |
| `label_definitions`, `label_set_versions`, `label_set_members` | the frozen label catalog (see [LABELS.md](LABELS.md)) |
| `labels` | future-outcome label vectors per closed 1m bar (partitioned monthly) |
| `label_computation_runs` | label-builder audit trail |
| `feature_vectors` | the local archive of live AltTech feature vectors (partitioned monthly) |
| `models`, `model_artifacts`, `model_metrics`, `model_walk_forward_runs`, `model_feature_compatibility`, `model_events` | the model registry (see [PROMOTION_GATES.md](PROMOTION_GATES.md)) |
| `retention_policies`, `retention_runs`, `storage_snapshots` | the database size budget (see [STORAGE_BUDGET.md](STORAGE_BUDGET.md)) |

`altml.labels` and `altml.feature_vectors` are `RANGE`-partitioned by month
via `altml.ensure_month_partitions()`; retention drops whole partitions, never
row-by-row `DELETE`.

## Applying the schema

```sh
sudo -u postgres scripts/db-bootstrap.sh   # one-time: role, database, FDW, schema
```

The script is idempotent and records the applied schema's checksum in
`altml.schema_version`. Re-running it after a `db/schema.sql` change re-applies
the (idempotent, `CREATE ... IF NOT EXISTS`) DDL and refreshes the checksum.
