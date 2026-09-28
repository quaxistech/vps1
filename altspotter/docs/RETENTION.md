# Retention

AltSpotter applies retention from the configured policies in [altspotter.example.yaml](../config/altspotter.example.yaml). The values below describe that example profile; a deployed configuration may choose different horizons.

Retention operates on whole time partitions. The configured target is the normal horizon, while the minimum is a protected floor used when storage pressure requires additional cleanup. Cleanup priority determines which eligible partitions are considered first.

## Example horizons

| Data family | Example target | Protected minimum |
|---|---:|---:|
| Raw trades, core assets | 60 hours | 2 hours |
| Raw trades, other assets | 24 hours | 2 hours |
| Order-book snapshots, core assets | 6 hours | 2 hours |
| Order-book snapshots, other assets | 4 hours | 2 hours |
| 1-second trade and book aggregates, core assets | 12 hours | 4 hours |
| 1-second trade and book aggregates, other assets | 8 hours | 4 hours |
| 1-minute candles | 280 days | 120 days |
| Derivatives context | 90 days | 45 days |
| 1-minute BBO and trade/book aggregates | 14 days | 7 days |
| Higher-timeframe candles | 400 days | 200 days |
| Funding settlements | 200 days | configured under operational retention |

Core assets are listed in the retention configuration and reconciled into the database at service startup.

## Backfill and retention

History depth and retention are validated together. The configured backfill must fit within the retention of the table it writes to. Funding settlements have the same relationship. The projected database footprint must also remain below the cleanup threshold.

These checks run through <code>altspotter config-check</code> and at service startup. See [Configuration](CONFIGURATION.md) for the relevant settings.

## Partitioning

| Data | Partitioning |
|---|---|
| Raw trades | Retention class, then hourly event-time partitions |
| Order-book snapshots | Retention class, then hourly event-time partitions |
| 1-second trade and book aggregates | Retention class, then daily time partitions |
| 1-minute and higher-timeframe series | Monthly time partitions |

Market-data retention removes expired partitions with partition drops. It does not delete individual market-data rows. Compact aggregates are advanced from raw-data watermarks before the corresponding raw partitions expire.

Low-volume operational tables use bounded age-based cleanup configured under <code>ops_retention</code>.

## Runtime

The <code>retention_manager</code> component applies policies during normal service operation. Use <code>altspotter retention-run --dry-run</code> to preview a one-shot pass and <code>altspotter inspect --retention</code> to view policies and recent runs. See [Operations](OPERATIONS.md).
