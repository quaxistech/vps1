# Database ownership

AltSpotter’s database is the authoritative store for the market data collected by this service. The schema in <code>db/migrations/</code> defines the tables and constraints.

## AltSpotter data ownership

AltSpotter owns:

- venue and canonical-instrument catalog records;
- candles, trades, order-book snapshots, BBO, derivatives context, and funding settlements;
- derived candle and microstructure aggregates;
- history coverage, detected gaps, venue quality, and connection state;
- retention policies and storage measurements.

Only AltSpotter writes these market-data records. Consumer roles receive read access through the interface assigned to their service.

## Cross-service storage

Each downstream service owns its own records and migrations. A consumer may retain derived features, predictions, decisions, or delivery state, but those records do not replace or mutate the source market data.

Cross-service reads are one-way and read-only. Storage ownership and access grants are established by each service’s deployment configuration.

## Schema changes

- Add or alter database structures through versioned migrations.
- Keep changes compatible with the consumer contract documented for the affected table or message.
- Preserve event time, canonical identity, source, and revision semantics.

For the AltSpotter table catalog, see [Database](../DATABASE.md). For collection and retention behavior, see [Market-data inventory](../MARKET_DATA_INVENTORY.md) and [Retention](../RETENTION.md).
