# Inter-service contracts

This document describes the AltSpotter boundary with its upstream venues and downstream consumers. Service-specific payload definitions remain owned by the producer.

## Venues to AltSpotter

| Item | Contract |
|---|---|
| Source | Public venue REST and WebSocket interfaces |
| Products | Configured linear USDT perpetual markets |
| Input | Venue-specific payloads |
| Normalization | Venue data is converted to AltSpotter domain and protobuf types |
| Persistence | PostgreSQL migrations define the durable schema |
| Identity | Canonical instrument identity is mapped to each venue’s symbol |
| Time | Venue event time is retained separately from local receive and persistence times |

## AltSpotter to consumers

| Item | Contract |
|---|---|
| Live updates | Core NATS subjects described in [NATS](../NATS.md) |
| Replayable events | AltSpotter-owned JetStream streams |
| Durable history | AltSpotter-owned PostgreSQL tables |
| Message schema | Versioned protobuf definitions under <code>proto/</code> |
| Corrected records | Revisions identify changed historical values |
| Missing values | Absence remains explicit; consumers must not replace it with zero |
| Ownership | Consumers read through their assigned role and do not write AltSpotter tables |

Use the [Consumer guide](../CONSUMER_GUIDE.md) for connection, subscription, and decoding guidance. The protobuf files and database migrations are the authoritative interface definitions.

## Cross-service principles

- A producer owns the meaning and version of its messages.
- Consumers preserve the producer’s identity, event time, and revision fields.
- Durable consumers use independent durable names and acknowledge after their own processing commits.
- PostgreSQL remains authoritative for durable market history; NATS transports updates and events.
