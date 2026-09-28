# Market-data inventory

This page describes the data families AltSpotter collects, derives, stores, and publishes. Deployment-specific row counts and current coverage are not part of this reference; inspect a running service for those values.

The supported instrument universe is configured in [symbols.yaml](../config/symbols.yaml) and resolved against the enabled venues. See [Global universe](GLOBAL_UNIVERSE.md).

## Market data

| Family | Source | PostgreSQL | NATS |
|---|---|---|---|
| Instrument catalog and contract specifications | Venue metadata and configured assets | Catalog, mappings, and specification history | Instrument and universe events |
| One-minute candles | Venue REST history and live candle feeds | <code>market_candles_1m</code> | Live updates and closed-bar events |
| Higher-timeframe candles | Derived from stored one-minute candles | <code>market_candles_agg</code> | Aggregated closed-bar events |
| Raw trades | Live venue trade feeds | <code>market_trades_raw</code> | Core NATS trade events |
| Order-book snapshots | Reconstructed live books, sampled at configured cadence | <code>market_orderbook_snapshots</code> | Snapshot and delta events |
| Best bid and ask | Live top-of-book updates | <code>market_bbo_1m</code> and the derivatives-context record | Market-context events |
| Derivatives context | Live mark, index, funding, open-interest, and ticker feeds; supported history sources | <code>market_derivatives_context_1m</code> | Market-context events |
| Funding settlements | Venue funding history and settlement events | <code>market_funding_settlements</code> | Funding events |
| Trade aggregates | Derived from raw trades | <code>market_trade_aggregates_1s</code> and <code>market_trade_aggregates_1m</code> | Not a separate raw-trade feed |
| Book aggregates | Derived from sampled order books | <code>market_book_aggregates_1s</code> and <code>market_book_aggregates_1m</code> | Not a separate book-delta feed |

Exact subjects, streams, and protobuf types are documented in [NATS](NATS.md) and the [Consumer guide](CONSUMER_GUIDE.md). Table definitions are documented in [Database](DATABASE.md) and enforced by the migrations in <code>db/migrations/</code>.

## Operational metadata

AltSpotter also stores catalog state, backfill coverage, detected gaps, rejected events, venue connection state, quality counters, retention policies, and storage measurements. These records describe collection and persistence state; they are not substitutes for market observations.

## Collection boundaries

- Supported products are linear USDT perpetual futures on the four configured venues.
- Historical collection uses the REST capabilities implemented by each adapter.
- Trades, order-book updates, BBO, and live context are collected from live feeds.
- Higher-timeframe candles and microstructure aggregates are derived locally.
- Absent venue fields remain absent. See [Market-data semantics](MARKET_DATA_SEMANTICS.md).
- Retention horizons are controlled by configuration; see [Retention](RETENTION.md).
