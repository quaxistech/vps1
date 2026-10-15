# Database

AltSignalQuality owns the `altsignalquality` PostgreSQL schema, in production
in its own `altsignalquality` database; the AltStrategy tables it reads are
`postgres_fdw` foreign tables in its local schema `altstrategy` (server
`altstrategy_srv`). The ordered schema is defined by
`db/migrations/0001_altsignalquality_schema.sql` and applied separately from
service deployment.

## Read-only upstream data

The service reads AltStrategy data used to resolve proposal metadata, point-in-
time context and matured outcomes. These tables remain owned by AltStrategy:

| table | use |
|---|---|
| `trade_signal_proposals` | live proposal lookup and dataset backfill |
| `strategy_candidates` | proposal-to-context association |
| `decision_context_snapshots` | point-in-time signal context |
| `market_outcomes` | resolved outcomes and cost-adjusted returns |
| `proposal_warnings` | warning features |
| `signal_training_examples` | upstream outcome-label provenance |

The service role has write access to its own schema and read access to the
upstream tables it consumes. It does not modify AltStrategy tables.

## Owned table groups

- **Contracts:** `feature_schemas`, `label_specs` store versioned feature and
  label definitions.
- **Evaluations:** `signal_evaluations` stores proposal identity, point-in-time
  inputs, validity decision, optional diagnostic score and publication state.
- **Dataset:** `training_examples`, `outcome_priors` and `prior_state` hold
  matured examples and as-of aggregates.
- **Models:** `dataset_versions`, `models`, `model_artifacts`, `model_metrics`,
  `model_walk_forward_runs`, `model_feature_compatibility`,
  `model_calibration`, `model_events` and `training_runs` preserve optional
  training lineage.
- **Retention:** `retention_policies` and `retention_runs` define and record
  per-table retention.

## Storage invariants

The schema enforces version identity for feature and label contracts, score
ranges, evaluation and publication relationships, model lifecycle vocabulary,
and label-maturation constraints. Deterministic identifiers and conflict-safe
writes make repeated message delivery and dataset passes idempotent.

Migrations are applied by `scripts/db-bootstrap.sh`, not by the service. The
migration ledger is stored in the owned schema so version identifiers do not
collide with other services' migrations.

## Default retention

| table | days |
|---|---:|
| `signal_evaluations` | 120 |
| `training_examples` | 400 |
| `model_events` | 365 |
| `training_runs` | 90 |

The `retention_policies` table is the runtime source of truth and can be
reviewed or updated without changing the schema.
