# AltML

AltML is the market-forecasting service for Bitget USDT perpetuals. It consumes
live one-minute feature vectors from AltTech over NATS, builds point-in-time
labels from AltSpotter market history, trains CPU LightGBM models inside the
service, and publishes versioned forecasts over NATS. Economic estimates are
reported with forecasts; trading and execution decisions belong downstream.
One binary, one systemd unit, one configuration file:

```bash
systemctl start altml.service        # or: altml run --config /etc/altml/altml.yaml
```

On first start there is no model yet: AltML serves safely in
`DETERMINISTIC_ONLY` mode, accumulating labeled history, until it has enough
rows to train (`training.min_rows`). From then on it keeps re-training itself
on a rolling window using purged walk-forward validation, market-quality
promotion gates, and champion-challenger comparison.

AltML **never** publishes a trade signal, sends an order, or sets a
stop-loss/take-profit. Everything downstream of a forecast belongs to other
services.

## Forecast contracts and ownership

The implemented `econ` mode publishes horizon-specific LONG/SHORT/NEUTRAL
market forecasts with excursion magnitudes and explicit cost/calibration
metadata (`PredictionV1`, schema 4). The retained `legacy` mode interprets older
binary direction models. Features and aligned sources are pinned by the
registered recipe; optional upstream macro/cross-venue publication does not
prove a model consumes it. No forecast is downstream trading approval.

AltML owns its feature archive, labels/datasets, artifacts, model registry and
lifecycle events in the `altml` schema. It reads upstream data through read-only
foreign tables and live versioned events. [Feature contract](docs/FEATURE_CONTRACT.md)
and [configuration](docs/CONFIGURATION.md) define compatibility and mode selection.
[Recorded registry evidence](docs/archive/registry-2026-10-15.md) retains the
original observed model/recipe identities. Runtime/model availability is
**NOT VERIFIED DURING DOCUMENTATION-ONLY AUDIT**.

```text
altml run             the service: label-builder + trainer + model-registry +
                      inference + storage-guard, one supervised process
altml train           one manual training cycle (diagnostics/CI)
altml nats-setup      manual JetStream provisioning (also runs inline at startup)
altml model-validate  checks a model artifact against its recorded
                      predictions
```

## Layout

```
src/        C++23 sources: core runtime, components (incl. the native trainer), CLI
proto/      altml.v1 wire schema + vendored alttech.v1 producer contract
db/         db/schema.sql — the one, final AltML-owned schema
config/     altml.example.yaml, altml.env.example
deploy/     systemd units (altml.service + hardening drop-in)
scripts/    release build, provisioning, database bootstrap, unit installer
tests/      C++ unit tests, release-layout contract test
docs/       design and operations documentation (see below)
```

## Documentation

- [docs/README.md](docs/README.md) — complete documentation index
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — the supervised components, contracts, data flow
- [docs/DATABASE.md](docs/DATABASE.md) — the `altml` schema, foreign tables into AltSpotter/AltTech, retention
- [docs/LABELS.md](docs/LABELS.md) — the label catalog, leakage discipline
- [docs/FEATURE_CONTRACT.md](docs/FEATURE_CONTRACT.md) — the offline/live feature-identity invariant
- [docs/PROMOTION_GATES.md](docs/PROMOTION_GATES.md) — the gates a candidate model must pass to go ACTIVE
- [docs/FIRST_START_BOOTSTRAP.md](docs/FIRST_START_BOOTSTRAP.md) — behavior with no model yet
- [docs/AUTONOMOUS_LIFECYCLE.md](docs/AUTONOMOUS_LIFECYCLE.md) — the trainer's self-scheduling
- [docs/ARTIFACTS.md](docs/ARTIFACTS.md) — model artifact layout and integrity
- [docs/STORAGE_BUDGET.md](docs/STORAGE_BUDGET.md) — the database size budget and per-table retention
- [docs/NATS.md](docs/NATS.md) · [docs/CONFIGURATION.md](docs/CONFIGURATION.md) · [docs/SECURITY.md](docs/SECURITY.md) · [docs/TESTING.md](docs/TESTING.md)
- [docs/INTEGRATION.md](docs/INTEGRATION.md) · [docs/OPERATIONS.md](docs/OPERATIONS.md) — connecting consumers, running the service

## Build and test

Baseline: Ubuntu 24.04, CMake 3.28, Ninja, GCC, C++23. nats.c 3.13.0 and
LightGBM 4.6.0 are fetched at pinned revisions. System libraries: OpenSSL,
Protobuf, libpq/libpqxx, systemd, yaml-cpp, spdlog, fmt, nlohmann-json,
Catch2, libgomp. No Python toolchain is required anywhere.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

`scripts/build-release.sh` builds an immutable release archive (binary,
schema, proto, configs, units, checksums) after all tests pass.

## Database

AltML owns exactly one PostgreSQL database (schema `altml`) and never writes
to AltSpotter's or AltTech's databases. It reads their candle/instrument and
feature-catalog tables **read-only** through `postgres_fdw` foreign tables,
and archives every live AltTech feature vector it consumes into its own
`altml.feature_vectors` table — AltTech keeps a bounded live history. AltML retains the configured training
lookback in its own archive. See [docs/DATABASE.md](docs/DATABASE.md).

```sh
scripts/db-bootstrap.sh   # one-time: creates the altml database/role, the
                          # read-only reader roles in AltSpotter/AltTech, the
                          # FDW wiring, and applies db/schema.sql
```

## Configuration

One file: `/etc/altml/altml.yaml` (see
[config/altml.example.yaml](config/altml.example.yaml)) — common connection
keys at the root, then `project:` (the adapter), `inference:`,
`label_builder:`, `training:` (the in-process trainer), `model_registry:` and
`storage:` (database size budget + retention). Secrets are referenced as
`${ALTML_*}` and live in `/etc/altml/common.env`; a missing variable is a
startup error. Never put credentials in the repository or a release archive.

## Deployment

- one installed tree: `/opt/altml/{bin,lib,share}` (no `releases/`, no `current`, no backups; rollback = redeploy an earlier build artifact);
- data: `/var/lib/altml/models`; service account `altml`;
- exactly one unit: `altml.service` (`Type=notify`, `WatchdogSec=60`,
  `Restart=on-failure`) — no job oneshots, no timers; training is scheduled by
  the service itself.

`scripts/provision.sh` creates the account and filesystem;
`scripts/db-bootstrap.sh` provisions the database; `scripts/install-systemd-units.sh`
installs the unit. Operations and journald filtering:
[docs/OPERATIONS.md](docs/OPERATIONS.md).

## Service boundary

- reads AltSpotter's candle history and AltTech's feature-set catalog,
  read-only, through `postgres_fdw`;
- consumes AltTech's live `alttech.feature.venue.bitget.>` feature vectors over
  NATS (`ALTTECH_ANALYTICS` stream, durable `inference-features`);
- publishes `prediction.bitget.<symbol>.1m.<model_version>` on its own
  `ALTML_ANALYTICS` stream;
- publishes model lifecycle events under `evt.model.*` on its own
  `ALTML_CONTROL` stream.

No AltML name, subject or schema is shared with, or depends on, any other
service's internals — enforced by `tests/unit/test_wire_contract.cpp` and
`tests/shell/test_ml_release_contract.sh`.

## Documentation index

See [docs/README.md](docs/README.md) for architecture, data contracts,
configuration, operations, and model lifecycle references.
