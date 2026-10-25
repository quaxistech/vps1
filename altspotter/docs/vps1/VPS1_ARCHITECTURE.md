# VPS1 architecture

VPS1 is the market-intelligence side of the system. It observes public market data, derives features and predictions, and publishes market signals. It does not place or manage trades. The trading system consumes signals through the documented gateway boundary.

## Services

| Service | Responsibility |
|---|---|
| AltSpotter | Venue market data, history, catalog, quality, and retention |
| AltTech | Market features, microstructure, regimes, and structure |
| AltMacro | Point-in-time macro and cross-asset data |
| AltML | Labels, datasets, model evaluation, registry, and inference |
| AltStrategy | Market setup detection, proposals, and market outcomes |
| AltSignalQuality | Signal quality evaluation |
| AltSignalGateway | Durable signal journal and client delivery |

The service-level designs and signal boundary are described in [VPS1 market-intelligence boundary](VPS1_MARKET_INTELLIGENCE.md).

## Data path

    venues ──► AltSpotter ──► AltTech ──► AltML
                     │            │          │
                     └────────────┴──────► AltStrategy
                                              │
                                              ▼
                                    AltSignalQuality
                                              │
                                              ▼
                                     AltSignalGateway
                                              │
                                         TLS client

AltSpotter owns the normalized market-data contract. Downstream services consume the documented PostgreSQL and NATS interfaces; they do not write into AltSpotter’s market-data tables.

## Event and storage principles

- PostgreSQL is the durable record for stored market data.
- JetStream carries replayable events; Core NATS carries live updates.
- Producers own the streams and schemas they publish.
- Consumers acknowledge durable messages after their own durable work completes.
- Event time and revision metadata are preserved across derived records.

See [Inter-service contracts](CONTRACTS.md), [Database ownership](DATABASE_ARCHITECTURE.md), and [Temporal model](TEMPORAL_MODEL.md).

## External context branch

AltMacro owns external market/reference/release vintages. AltTech's sixth,
macro module reads these through `altmacro_ref` and publishes measured global
context. Feature recipes determine which models consume it; publication alone
does not establish model usage. The main diagram is the crypto path, not a claim
that AltMacro or every service lies in one linear runtime cascade.
