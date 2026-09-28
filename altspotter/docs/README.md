# Documentation

This directory is the canonical documentation for AltSpotter. Start with the repository [README](../README.md) for scope, build, and launch instructions.

## Build and operate

| Topic | Document |
|---|---|
| Configuration files, defaults, and validation | [Configuration](CONFIGURATION.md) |
| Build, stage, and activate a release | [Deployment](DEPLOYMENT.md) |
| Routine commands and service state | [Operations](OPERATIONS.md) |
| Storage limits and persistence modes | [Storage budget](STORAGE_BUDGET.md) |
| Data retention and partition policy | [Retention](RETENTION.md) |
| Security boundaries | [Security](SECURITY.md) |
| Automated test suites | [Testing](TESTING.md) |

## Architecture and data contracts

| Topic | Document |
|---|---|
| Components and data flow | [Architecture](ARCHITECTURE.md) |
| Consumer access to PostgreSQL, NATS, and protobuf | [Consumer guide](CONSUMER_GUIDE.md) |
| Database tables and ownership | [Database](DATABASE.md) |
| NATS subjects and JetStream streams | [NATS](NATS.md) |
| Data families and persistence paths | [Market-data inventory](MARKET_DATA_INVENTORY.md) |
| Units, timestamps, nullability, and revisions | [Market-data semantics](MARKET_DATA_SEMANTICS.md) |
| Quality states and coverage records | [Data quality](DATA_QUALITY.md) |
| Instrument eligibility | [Global universe](GLOBAL_UNIVERSE.md) |
| Order-book normalization and sampling | [Order books](ORDERBOOKS.md) |

## Venue adapters

[Exchange overview](EXCHANGES.md) · [Bitget](BITGET.md) · [Binance USDⓈ-M](BINANCE_USDM.md) · [Bybit Linear](BYBIT_LINEAR.md) · [OKX swaps](OKX_USDT_SWAP.md)

## Cross-service context

The following references describe interfaces between AltSpotter and the other VPS1 services. AltSpotter-specific setup and runtime behavior is documented above.

- [VPS1 architecture](vps1/VPS1_ARCHITECTURE.md)
- [Inter-service contracts](vps1/CONTRACTS.md)
- [Database ownership](vps1/DATABASE_ARCHITECTURE.md)
- [Temporal model](vps1/TEMPORAL_MODEL.md)
- [Data leakage model](vps1/DATA_LEAKAGE_MODEL.md)
- [Data and model lineage](vps1/ML_LINEAGE.md)
- [Market-intelligence boundary](vps1/VPS1_MARKET_INTELLIGENCE.md)

## Documentation rule

Example values describe the checked-in configuration, not necessarily a live host. Deployment-specific state such as active instruments, data coverage, storage use, and consumer counts must be read from that deployment.
