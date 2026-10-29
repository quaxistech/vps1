# Database

PostgreSQL database `altspotter`, UTC throughout, `NUMERIC` money (never binary
float). Every market key includes `exchange_id` (+ `instrument_id`, time).

The whole schema is one file — [`db/migrations/0001_schema.sql`](../db/migrations/0001_schema.sql).
`altspotter migrate` applies it in a transaction, records it in
`schema_migrations` with a SHA-256 checksum, serializes concurrent runs with a
PostgreSQL advisory lock, and finally ensures candle partitions cover
`[now-120d, now+3mo]`. Applied migration files are immutable: editing one after
it has been applied is a hard error, so a schema change ships as a new
`NNNN_name.sql` file.

## Reference / catalog

- `market_types`, `exchanges` — venue registry; ids are fixed and mirror the
  proto `Exchange` enum (1 bitget, 2 binance_usdm, 3 okx, 4 bybit).
- `instruments`, `canonical_pairs` (`id == instrument_id`),
  `exchange_instruments`, `canonical_pair_exchange_map`, `instrument_spec_versions`.
- `global_observation_universe`, `global_universe_events` — four-venue
  eligibility and its transitions.

## Market data (exchange-aware)

- `market_candles_1m`, `market_candles_agg` — monthly RANGE partitions.
- `market_derivatives_context_1m`, `market_bbo_1m` — monthly RANGE partitions.
- `market_trade_aggregates_1s/_1m`, `market_book_aggregates_1s/_1m` — the `_1s`
  tables are LIST(retention_class) → **hourly** RANGE, the `_1m` tables monthly.
- `market_funding_settlements`.
- `market_trades_raw`, `market_orderbook_snapshots` — LIST(retention_class) →
  **hourly** RANGE partitions (ephemeral, dropped by retention).

Partition granularity follows the retention horizon, not the table: retention
can only drop a partition whose **upper** bound is already in the past, so a
table kept for hours needs hourly partitions or nothing can ever be reclaimed
within the day. See [RETENTION](RETENTION.md).

## Operational

- `history_coverage`, `history_gaps`, `ingestion_runs`, `rejected_market_events`.
- `exchange_connections`, `exchange_data_quality`, `exchange_clock_offsets`.
- `retention_policies`, `retention_runs`, `retention_core_assets`,
  `storage_budget_snapshots`, `storage_table_statistics`.

## Partition helpers

- `altspotter_ensure_candle_partitions(from, to)` — monthly 1m/agg partitions.
- `altspotter_ensure_time_partitions(parent, 'day'|'month', from, to)` — generic.
- `altspotter_partition_bounds(parent)` — child partitions + bounds (used by
  retention to drop whole partitions).
- `altspotter_ensure_microstructure_partitions()` — daily raw partitions.

Retention only ever **drops whole partitions**; it never issues row-level
`DELETE`. See [RETENTION](RETENTION.md) and [STORAGE_BUDGET](STORAGE_BUDGET.md).

Candle UPSERT revisions compare effective persisted values, including optional
trade/taker quantities, close time, closed state, source and quality. Aggregate
minute-count corrections also increment revision. Absent optional values retain
known values; identical effective rows are no-ops. A local receipt timestamp
alone is not a semantic revision. Source corrections are semantic provenance
changes. Existing natural keys, revision counters and retention are unchanged.
