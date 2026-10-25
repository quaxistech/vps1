# AltML architecture

AltML is a single service binary, `altml`, and the direction-forecasting core
of a Bitget USDT-perpetual trading stack. It answers one question — what is
the probability the next 15-minute bar moves up, down, or flat — with a
calibrated, native LightGBM model it trains and re-trains on itself. It never
publishes a trade signal, sends an order, or sets a stop-loss/take-profit;
those belong to services downstream of this forecast.

## Position in the data flow

```
AltSpotter (4-venue market data; AltML reads only its Bitget 1m candles)
    → AltTech (Bitget analytics; AltML reads only its Bitget "talib-core" feature vector)
    → AltML   horizon forecasts (econ LONG / SHORT / NEUTRAL; legacy P_UP / P_DOWN)
    → downstream strategy/execution services
```

AltML reads AltSpotter's and AltTech's PostgreSQL data **read-only**, through
`postgres_fdw` foreign tables inside its own database (see
[DATABASE.md](DATABASE.md)); it never joins their live traffic beyond that,
and it owns no table any other service depends on.

## Process model — one supervised process

`altml run` provisions the JetStream topology inline, then runs five
supervised components on their own threads under one `ShutdownToken`.
Sequential fail-fast `init()` runs before `READY=1`; a failed component stops
the whole process (exit 1) so systemd `Restart=on-failure` restarts everything
together.

```
altml run
├── label-builder     future-outcome labels for closed Bitget 1m bars,
│                     computed from AltSpotter candles (postgres_fdw)
├── trainer           native in-process LightGBM training, promotion gates,
│                     champion-challenger, weekly self-scheduling
├── model-registry    watches altml.models for status transitions, publishes
│                     evt.model.* lifecycle events
├── inference         consumes AltTech's live feature vectors, archives them,
│                     runs the ACTIVE model, publishes predictions
└── storage-guard     database size budget + per-table retention enforcement
```

## Native in-process training

There is no Python and no external job: the trainer component trains LightGBM
through the C API inside `altml run`. First start bootstraps from whatever
labeled history the database holds (`training.min_rows` gate; until then the
service serves safely in `DETERMINISTIC_ONLY` mode). Afterwards it re-trains on
a rolling `training.lookback_days` window at the configured cadence:
timestamp-grouped expanding walk-forward with a purged embargo, early stopping
on an embargoed validation slice, market-quality gates (proper scoring, base-rate skill and fold consistency),
economic diagnostics, a champion-challenger comparison on
the same held-out recent slice, and a write/read parity self-check before the
transactional ACTIVE flip (see [PROMOTION_GATES.md](PROMOTION_GATES.md)).
Training is resource-bounded (threads, memory estimate, wall-clock budget,
load-average/free-memory guards) and defers with
`JOB_DEFERRED_RESOURCE_PRESSURE` when the host is busy — live inference always
has priority. `altml train` runs one manual cycle for diagnostics/CI.

## Why AltML keeps its own feature archive

AltTech's live `venue_feature_vectors` table is retained for only a few days
(sized for live order attribution, not model training). AltML's rolling
93-day training window would have nothing to read if it depended on that
table directly, so the inference component archives every valid vector it
consumes from the live NATS stream into its own `altml.feature_vectors`
table as it arrives — real, observed market data, not backfilled or
synthesized. See [DATABASE.md](DATABASE.md).

## Contracts

- **Input:** AltTech's live `alttech.v1.FeatureVector` ("talib-core:1"),
  consumed over NATS on `alttech.feature.venue.bitget.>`. A vector that fails
  the freshness, schema-hash or feature-count check is dropped, never guessed
  at. See [FEATURE_CONTRACT.md](FEATURE_CONTRACT.md).
- **Output:** `altml.v1.PredictionV1` on `prediction.bitget.<symbol>.1m.<version>`;
  model lifecycle events (`evt.model.*`) on `ALTML_CONTROL`. See [NATS.md](NATS.md).
- **Storage:** the dedicated `altml` PostgreSQL schema
  ([../db/schema.sql](../db/schema.sql)) plus model artifacts under
  `/var/lib/altml/models`. See [DATABASE.md](DATABASE.md) and
  [ARTIFACTS.md](ARTIFACTS.md).

## Safety boundary

No direct exchange connections, no private exchange credentials, no trade
signal, no order execution, no final stop-loss/take-profit, no Docker.
Abstention (serving no prediction) is a normal, safe result, not a failure.
See [SECURITY.md](SECURITY.md).

## Source layout

```
src/main.cpp              CLI dispatcher (run | train | nats-setup | model-validate)
src/altml/core/           shared runtime: config, logging, shutdown, readiness,
                          time, hashing, PostgreSQL pool, NATS client/codec
src/altml/components/     supervised components + pure unit-tested logic headers
src/altml/cli/            subcommand entry points
proto/                    altml.v1 + vendored alttech.v1 producer contract
db/schema.sql             the one, final AltML-owned schema
```

## Cost estimate and forecast semantics

Each forecast carries an estimate of round-trip cost at its own T0. The estimate
combines the configured taker fees, the Bitget spread available at T0, and an
explicit impact assumption; it records when the spread fallback is used. This
estimate accompanies the market forecast and does not make a trading decision.

The same cost inputs are used by the economic label and inference paths. Model
calibration and policy metadata are loaded through the shared calibration
record parser. `altml replay --model-id ID --from-ms START --to-ms END` reruns
stored predictions through the feature, model, calibration, cost, estimate, and
policy stages.

AltML predicts market outcomes by horizon. Promotion uses out-of-sample market
forecast quality and walk-forward consistency. Economic results are reported
as diagnostics. A prediction records its calibration verdict, label definition,
forecast horizon, magnitude estimates and source, and model role. A probability
is marked calibrated only when it passes the configured holdout calibration
test; the verdict does not imply profitability or execution approval.
