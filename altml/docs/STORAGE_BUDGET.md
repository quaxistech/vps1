# Storage and retention

AltML manages only its own PostgreSQL schema and model-artifact directory. The
example configuration sets a 60 GB soft budget, a 66 GB hard limit, a five
minute measurement interval, and a cap of five retired or rejected models.
These are example deployment values; set limits for the host allocation.

## Table retention

`altml.retention_policies` in `db/schema.sql` is the source of truth:

| Data | Target | Minimum | Purpose |
|---|---:|---:|---|
| `feature_vectors`, `labels` | 40 days | 30 days | Training lookback plus fold margin |
| `model_predictions` | 60 days | 40 days | Compare model forecasts with later outcomes |
| `label_computation_runs` | 30 days | 7 days | Recent processing metadata |
| `model_events` | 180 days | 60 days | Model lifecycle record |

Model rows and artifacts outside the active set are retained by count, using
`storage.retired_model_keep_count`.

## Enforcement

The storage guard measures database size, records a snapshot, applies target
retention, and prunes old model records. If the database reaches its hard limit,
it reduces each policy toward its minimum. Each run is recorded in
`altml.retention_runs` and measurements in `altml.storage_snapshots`.
