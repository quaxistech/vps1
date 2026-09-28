# AltSpotter

AltSpotter is a single-process market-data service for linear USDT perpetual futures. It collects live and historical market data from Bitget, Binance USDⓈ-M, Bybit Linear, and OKX swaps, validates and stores it, and publishes versioned events for downstream consumers.

Spot, inverse, and delivery markets are outside the supported scope.

| Venue | Product | Code |
|---|---|---|
| Bitget | USDT-M perpetual | <code>bitget</code> |
| Binance | USDⓈ-M perpetual | <code>binance_usdm</code> |
| Bybit | Linear USDT perpetual | <code>bybit</code> |
| OKX | USDT-margined swap | <code>okx</code> |

Configured instruments enter the observation universe only when they are active on every enabled venue. A temporary venue outage does not change an instrument’s listing status.

## Data and interfaces

AltSpotter exposes market data through:

- **NATS and JetStream** for live updates and replayable events.
- **PostgreSQL** for durable market history and service metadata.
- **Protocol Buffers** in <code>proto/</code> for versioned message payloads.

All venue-specific payloads are normalized before publication. Database migrations and protobuf definitions are the authoritative interface contracts.

## Runtime

The <code>altspotter</code> executable provides these commands:

| Command | Purpose |
|---|---|
| <code>run</code> | Start the service components for enabled venues. |
| <code>migrate</code> | Apply database migrations and prepare partitions. |
| <code>nats-init</code> | Provision the service’s JetStream streams and consumers. |
| <code>config-check</code> | Validate configuration and retention constraints. |
| <code>inspect</code> | Report instrument, connection, data-quality, coverage, and storage state. |
| <code>retention-run</code> | Run a one-shot retention pass. |
| <code>version</code> | Print build and version metadata. |

The service runs venue adapters, history collection, storage writers, aggregation, integrity monitoring, and retention under one supervisor. See [Architecture](docs/ARCHITECTURE.md) for component boundaries and data flow.

## Build and test

Requirements: C++23, CMake 3.28 or newer, GCC, and Ninja. The release preset enables the test suite.

    cmake --preset release
    cmake --build --preset release
    ctest --test-dir build/release --output-on-failure

The executable is produced at <code>build/release/bin/altspotter</code>. The integration suite requires a NATS server and a PostgreSQL test DSN; see [Testing](docs/TESTING.md).

## Run locally

Set the PostgreSQL and NATS connection values for the local environment, then use the example configuration:

    B=build/release/bin/altspotter
    $B migrate --migrations db/migrations
    $B nats-init
    $B config-check --config config/altspotter.example.yaml --common config/common.example.yaml
    $B run --config config/altspotter.example.yaml --common config/common.example.yaml

The example files contain defaults for development. Production configuration and secrets belong under <code>/etc/altspotter/</code>, outside the source tree.

## Deployment

Deployment units and scripts are in <code>deploy/</code>. Build artifacts on the build host, stage a release with <code>deploy/scripts/publish.sh</code>, and activate it on the target using the installer command printed by the script. See [Deployment](docs/DEPLOYMENT.md).

## Documentation

[Documentation index](docs/README.md) · [Consumer guide](docs/CONSUMER_GUIDE.md)

The index links to configuration, operations, data contracts, storage, security, testing, and venue-specific references. Cross-service architecture and event contracts are grouped under [docs/vps1/](docs/vps1/VPS1_ARCHITECTURE.md).

Runtime counts, current coverage, and deployed retention policies vary by installation. Use the service inspection commands for live state; the checked-in example configuration is the reference for documented example values.
