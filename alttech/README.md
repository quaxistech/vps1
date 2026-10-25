# AltTech

AltTech turns the four-venue USDT-perpetual futures market data collected by
[altspotter](../altspotter) into durable, reproducible analytics: per-venue
feature vectors, microstructure, market structure and regimes, plus the
cross-venue consensus over all four venues.

One process, `alttech-service`, hosts six supervised modules:

| module | what it produces |
|---|---|
| `feature` | the full TA-Lib feature vector — 146 of the library's 161 functions, 995 columns — for the primary venue on 1m…1d, plus the lighter reference-venue vectors |
| `microstructure` | trade and order-book features over 1s…1m windows |
| `structure` | support/resistance zones with a lifecycle |
| `regime` | deterministic market regimes |
| `macro` | point-in-time external-series features from AltMacro through read-only foreign tables |
| `cross_venue` | event-time alignment, quality-weighted consensus, divergence, lead-lag, the global regime and the consensus zones projected onto the primary venue |

**Bitget** is the single primary trading venue; **Binance USDⓈ-M**, **Bybit
linear** and **OKX USDT swap** are reference venues that exist to give the
cross-venue analytics a second, third and fourth opinion. Spot markets are out
of scope.

AltTech holds no exchange credentials and connects to no exchange. It reads the
PostgreSQL tables and NATS subjects that altspotter publishes, computes
analytics, writes them into its own `alttech` PostgreSQL schema, and publishes
protobuf events on subjects under `alttech.`. Consumers need no client library:
the wire contract is the `alttech.v1` protobuf package in [proto/](proto/).

The observed pair set is not configured. It is read from altspotter's global
observation universe — the set of pairs active on all four venues — and AltTech
never mixes two venues' candles into one synthetic bar.

## Processing, configuration and recovery

A closed-bar event waits for AltSpotter's matching durable bar/revision before
feature computation; the feature vector commits before ACK. Other modules use
bounded owner-read sweeps. Natural keys include venue, bar/revision and feature
version; deterministic event IDs make repeated windows idempotent. Warm-up
restores from trailing durable history rather than serving a cold engine as
ready. Reference-venue absence degrades consensus quality without fabricating
agreement or blending venues into a synthetic bar.

[Configuration](docs/CONFIGURATION.md) selects module/timeframe/runtime behavior;
the producer's universe and frozen feature contracts remain authoritative.
[Architecture](docs/ARCHITECTURE.md) defines module-fatal versus transient
failures, coordinated shutdown and all-module readiness/watchdog behavior.
[Temporal rules](../altspotter/docs/vps1/TEMPORAL_MODEL.md) and
[cross-venue semantics](docs/CROSS_VENUE.md) qualify downstream availability.
AltML/Strategy consume analytics; AltTech cannot place a trade or alter their
model/setup decisions. [Operations](docs/OPERATIONS.md) owns diagnostic guidance.

## Documentation

Start with the [documentation index](docs/README.md). Consumer-facing database,
NATS and protobuf guidance is in [connect.md](connect.md).

## Repository layout

```text
src/
  app/                process entry point
  application/        lifecycle and module supervision
  modules/            feature, microstructure, structure, regime, cross_venue, macro,
                      venue_lite (the per-venue sweeps)
  config/             configuration model and validation
  messaging/          NATS adapter and protobuf codec
  persistence/        PostgreSQL adapter
  domain/             market types, venue identity, exact decimals
  talib_features/     the TA-Lib feature engine
  common|logging|time shared runtime facilities
  include/            internal headers
  tools/              schema, NATS topology, config check, analytics ops, version
proto/                the alttech.v1 wire contract
db/schema.sql         the complete alttech schema
config/               deployable configuration examples
deploy/                systemd units and the publish script
tests/                unit and contract tests
docs/                 documentation
```

## Build

CMake 3.28, Ninja, GCC and C++23. PostgreSQL, libpqxx, Protobuf, systemd,
yaml-cpp, spdlog, fmt, nlohmann-json, OpenSSL and Catch2 development packages
are required. nats.c 3.13.0 and TA-Lib 0.7.1 are fetched during configuration
with pinned SHA-256 hashes.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

The installed runtime is one daemon, `alttech-service`, plus the operational
tools `alttech-schema`, `alttech-nats-setup`, `alttech-config-check`,
`alttech-analytics` and `alttech-version`.

Deployment ships the built binaries; the target carries no source and no
toolchain. `deploy/publish.sh` verifies the artifact's ABI against the target
before installing it:

```sh
deploy/publish.sh root@host 1.0.0
```

See [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md) for the whole procedure.
