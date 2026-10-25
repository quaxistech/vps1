# altstrategy

**AltStrategy is the market setup and proposal service in VPS1.** It consumes
four-venue analytics and probabilistic ML forecasts,
runs a sixteen-strategy catalogue, ranks candidates adaptively, and publishes every
candidate, ranking and concrete LONG/SHORT **trade proposal** — with entry,
stop, take-profit, expected fill time, entry timeout and a ≤48h time-stop. It
stores every valid signal and plan, **computes its own market outcomes** from
AltSpotter history, and exports immutable training snapshots for downstream
SignalQuality training. It produces **decisions, not orders**: no position size,
leverage, balance, or exchange order id ever appears on its contracts.

One binary, one process, one systemd unit — `altstrategy-service`.

## Architecture

Event-driven. The single process reacts to upstream events and runs the pipeline
the instant a coherent `DecisionContext` is available for a pair; an internal
maintenance scheduler runs the periodic self-contained jobs. No fixed-cadence
stage cascade.

```
altstrategy-service
├── context_assembler          AltTech + AltSpotter + AltML → versioned DecisionContext
├── strategy_catalogue         sixteen strategies, ten edge families, lifecycle-gated
├── strategy_engine            runs every eligible strategy → all candidates
├── strategy_ensemble          direction / agreement / family independence
├── adaptive_strategy_allocator Bayesian shrinkage + time decay → adaptive weight
├── strategy_ranking           champion + challenger (shadow) policies
├── trade_plan_generator       AGGRESSIVE / BALANCED / CONSERVATIVE geometries
├── altml_candidate_evaluator  CandidateEvaluationRequest → CandidateEvaluation
├── proposal_publisher         grades, warnings, the single INVALID gate
├── signal_archive             persists every candidate / ranking / plan / proposal
├── market_outcome_tracker     self-computes outcomes from raw trades → 1m candles
├── training_snapshot_exporter FULL / INCREMENTAL Parquet* snapshots + manifest
├── execution_feedback_consumer optional execution feedback (never required; nothing publishes)
├── counterfactual_evaluator   non-selected candidate outcomes + ranking regret
├── outcome_attribution        strategy / ranking / geometry / ML / cost / execution
├── strategy_drift_monitor     STABLE / WATCH / DEGRADED / STRUCTURAL_BREAK
├── parameter_calibrator       bounded stop/target/timeout recalibration
├── maintenance_scheduler      outcome maturation, posteriors, drift, snapshots, retention
└── lifecycle_coordinator      versioned status transitions with rollback
```

\* On hosts built without `libarrow`/`libparquet`, the exporter emits validated
CSV columnar files with identical schema, manifest and SHA-256 checksums; the
Parquet backend activates automatically where the libraries are present.

## Boundary

AltStrategy **does not** size positions, choose leverage, check balances, place
orders, or manage positions. `recommended_for_execution` is false on every
proposal: whether to act is the downstream risk / sizing / execution layer's
decision alone. The only thing
AltStrategy blocks is a **technically INVALID** proposal (bad price-side
geometry, expired context, NaN, holding > 48h, …); weak ML/quality signals are
published as warnings, never hard-rejected.

## Contracts

- Wire: `altstrategy.v1` protobuf (`proto/decision.proto`), consuming the
  vendored `alttech.v1` analytics and `altml.v1` forecast/evaluation contracts.
- Storage: the AltStrategy-owned `altstrategy` PostgreSQL schema
  (`db/migrations/`). It never modifies the AltSpotter / AltTech / AltML schemas
  (read-only inputs). In production it is a schema of its own `altstrategy`
  database, which also holds the Gateway's `altsignalgateway` schema;
  AltSpotter's market history and instrument registry are read through
  `postgres_fdw` foreign tables ([docs/DATABASE.md](docs/DATABASE.md)).
- **Feature sets are pinned, not guessed.** AltTech ships compact ORDERED
  vectors and states their identity on every message. `context.*_feature_set` in
  the config pins that identity (`name`, `version`, `schema_hash`,
  `value_count`) and maps only the positions this engine reads. A vector whose
  identity does not match is **refused**, not decoded: position is meaningless
  relative to the wrong set, and a mislabelled feature is undetectable
  downstream. Re-pin from the producer's catalog after any AltTech feature-set
  change:

  ```sh
  psql -d alttech -c "SELECT position, output_name FROM alttech.feature_set_members
                      WHERE feature_set_version_id = 1 ORDER BY position"
  ```

  Quantities that are not TA-Lib outputs are read where they are measured, never
  from the indicator vector: price from the cross-venue consensus mid, spread /
  depth / trade flow from the `bitget_micro` microstructure window, funding from
  the cross-venue set. Composites the engine needs but no producer publishes
  (`atr_expansion`, `ema_compression`, `bb_width_20`, `vwap_dev`, `price_stall`)
  are derived in `context_assembler.cpp` with their formulas stated there.
- **One decision timeframe.** AltTech publishes a vector per timeframe on a
  single subject; `context.decision_timeframe_minutes` selects the one used, so
  every indicator in a context describes the same bar length.
- NATS: consumes `alttech.*`, `prediction.*`, `evt.universe.*` and the optional
  `signal.execution_feedback.*`. It sends AltML nothing — the relationship is
  publish/subscribe in one direction. It owns and publishes
  `strategy.candidate.*`, `strategy.ensemble.*`, `strategy.ranking.*`,
  `tradeproposal.*`, `signal.proposed.*`, `signal.market_outcome.*` and
  `archive.snapshot.created.v1`. Drift, lifecycle, calibration, plan variants
  and counterfactuals are **persisted, not published** — see `docs/NATS.md`.

## Publication authority and temporal recovery

A technically valid proposal is market information, not financial admission.
`recommended_for_execution` remains false; lifecycle names, economic diagnostics
and adaptive ranking cannot override VPS2 Risk. Native downstream entry filters
do not rewrite the original VPS1 setup, horizon, expiry or payload.

The owned `altstrategy` schema archives immutable context revisions, candidates,
rankings, plans/proposals, market outcomes and training provenance. A context
that cannot be archived cannot decide. Proposal/outbox commit precedes publishing;
restarts resume durable maintenance and publication progress. Market outcomes
mature independently of external execution feedback, with AMBIGUOUS and
DATA_INCOMPLETE retained explicitly.

[Configuration](docs/CONFIGURATION.md), [point-in-time rules](docs/POINT_IN_TIME_CORRECTNESS.md)
and [lifecycle](docs/STRATEGY_LIFECYCLE.md) define current selection and failure
semantics. Runtime strategy states are not inferred from compiled seeds or
historical registry snapshots. No production access occurred for this audit.

## Build

```sh
cmake --preset release          # RelWithDebInfo, tests ON
cmake --build --preset release
ctest --preset release          # unit + integration tests
```

The single binary is `build/release/bin/altstrategy-service`.

## Run

```sh
altstrategy-service --config /etc/altstrategy/altstrategy.yaml \
                    --common /etc/altstrategy/common.yaml
```

## Deploy

Independent, under `/opt/altstrategy`, native systemd, no Docker:

```sh
scripts/deploy.sh        # build → upload → install → symlink (does NOT start)
```

Deploy installs the immutable release, non-secret config, the systemd unit and
provisions `/var/lib/altstrategy/{archive,exports,cache}`. It never starts the
service, never runs migrations, and never touches other services' schemas or
streams. Start it once ready:

```sh
systemctl enable --now altstrategy.service
```

See `docs/` for architecture, integration, database, NATS, configuration,
operations, the signal archive, market outcomes, training snapshots, point-in-time
correctness and service independence.

On the strategies themselves:

- `docs/STRATEGY_ARCHITECTURE.md` — how a decision is assembled, and why absence
  is a value rather than a zero
- `docs/STRATEGY_CATALOGUE.md` — the sixteen hypotheses, their inputs and seeds
- `docs/STRATEGY_SOURCES.md` — where each hypothesis comes from and **what would
  falsify it**
- `docs/STRATEGY_RESEARCH.md` — the replay, walk-forward and ablation workflow
- `docs/STRATEGY_HORIZONS.md` — declared horizons and the 48-hour ceiling
- `docs/STRATEGY_LIFECYCLE.md` — what a lifecycle seed does and does not claim
- `docs/STRATEGY_ECONOMICS.md` — the cost model and where each cost enters

## Documentation index

See [docs/README.md](docs/README.md) for the complete service documentation.
