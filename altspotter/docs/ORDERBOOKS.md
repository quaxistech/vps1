# Order books

AltSpotter maintains venue-specific live books and persists sampled top-of-book depth. Venue sequence rules are applied before a reconstructed book is published as valid.

## Inputs and normalization

| Venue | Feed model | Sequence validation |
|---|---|---|
| Binance USDⓈ-M | REST snapshot followed by depth deltas | Update identifiers and previous-update continuity |
| Bybit Linear | Snapshot and delta updates | Update identifiers and snapshot reset semantics |
| OKX swaps | Snapshot and delta updates | Previous sequence identifier and checksum |
| Bitget | Complete top-of-book snapshots | Each message is treated as a standalone snapshot |

The local book is keyed by venue and instrument. A sequence gap, checksum mismatch, or crossed book prevents the affected state from being reported as valid. The runtime requests a fresh book state and publishes the new snapshot after synchronization.

## Persisted snapshot

The configured sampler stores a top-15 snapshot with best bid and ask, midpoint, microprice, spread, depth, imbalance, level count, and venue sequence metadata. Price and size levels preserve decimal values.

Sampling cadence is shared across venues through the retention-class settings. This keeps derived book aggregates comparable for the same configured class.

Snapshots are collected from live feeds; this service does not backfill historical order-book depth. See [Retention](RETENTION.md) for the configured storage windows and [Data quality](DATA_QUALITY.md) for published quality states.

The reconstruction behavior is covered by the order-book tests described in [Testing](TESTING.md).
