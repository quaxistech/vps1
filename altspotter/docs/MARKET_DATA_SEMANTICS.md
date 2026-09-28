# Market-data semantics

This document defines the meaning and representation of values exposed by AltSpotter. Consumers should not infer observations from missing data.

## Numeric values

Prices, quantities, volumes, rates, and spreads are stored as PostgreSQL <code>numeric</code> values and represented by the service’s decimal type. Counts and sequence values use integer columns. Order-book levels are stored as price/size pairs without converting their decimal text through a binary floating-point representation.

## Time

| Field | Meaning |
|---|---|
| <code>open_time</code> | Start of a candle interval |
| <code>close_time</code> | Exclusive end of the candle interval |
| <code>trade_time</code> | Venue event time for a trade |
| <code>snapshot_time</code> | Time associated with a sampled book |
| <code>funding_time</code> | Funding settlement time |
| <code>received_at</code>, <code>stored_at</code>, <code>updated_at</code> | Local receive or persistence timestamps |

Timestamps are stored in UTC. Candle intervals align to their timeframe. The Bitget daily-candle adapter requests UTC-aligned daily bars.

## Missing fields

A nullable field means the source did not provide an observation for that record or the field is not supported by that venue’s payload. It must not be replaced with zero or with a value from another source.

Examples include venue-specific trade counts, aggressor-volume splits, and fields that are unavailable on historical context rows. PostgreSQL NULL and unset protobuf optional fields preserve this distinction. See the per-venue references for payload differences.

## Provenance and revisions

Stored records retain their source and quality metadata where applicable. Historical candles, reconciliation updates, live venue data, and locally aggregated bars are distinguishable through their source fields.

A candle revision changes when a re-fetch changes the stored value. Reprocessing an unchanged record does not itself represent a data revision.

The derivatives-context source mask records which component fields were observed for a minute. A partially populated historical record therefore remains distinguishable from a complete live observation.

## Identity and deduplication

Canonical instruments are mapped to venue-specific symbols. Consumers should join by identifiers such as <code>exchange_id</code> and <code>instrument_id</code>, not by assuming that a symbol string has the same form on every venue.

| Data | Natural key |
|---|---|
| One-minute candles | Exchange, instrument, interval start |
| Higher-timeframe candles | Exchange, instrument, timeframe, interval start |
| Raw trades | Retention class, exchange, instrument, event time, venue trade ID |
| Order-book snapshots | Retention class, exchange, instrument, snapshot time |
| One-minute context and aggregates | Exchange, instrument, interval start |
| One-second aggregates | Retention class, exchange, instrument, interval start |
| Funding settlements | Exchange, instrument, settlement time |

Use the migrations in <code>db/migrations/</code> as the authoritative schema definition.
