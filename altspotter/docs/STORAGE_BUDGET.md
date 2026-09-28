# Storage budget

The <code>storage.hard_database_limit_bytes</code> setting defines the database-size limit used by the persistence gate. The example value is 40 GiB; choose the deployed value from the configured retention horizons and the capacity available to PostgreSQL.

The storage monitor compares database size with the configured ratios:

| State | Threshold in the example profile | Persistence behavior |
|---|---:|---|
| NORMAL | below 80% | Full persistence |
| WARNING | 80% | Full persistence |
| CLEANUP | 87% | Raw order-book persistence is disabled |
| CRITICAL | 93% | Raw trade persistence is also disabled |
| HARD_LIMIT | 100% | Persistence is restricted to core data |

The thresholds are configurable. The monitor interval in the example is 300 seconds.

## Persistence modes

The process-wide persistence gate is consulted by raw-data writers:

- CLEANUP stops writing sampled order-book snapshots.
- CRITICAL also stops writing raw trades.
- HARD_LIMIT retains core data such as candles and compact context.

Market-data publication over NATS remains separate from this database persistence gate. Storage-state snapshots are recorded in PostgreSQL and exposed through storage events.

## Capacity planning

Set the database limit below the disk capacity available to PostgreSQL. Leave room for indexes, vacuum, write-ahead logs, backups, and ingestion bursts. Database size and filesystem use are separate measurements.

Capacity depends on the number of eligible instruments, sampling cadence, order-book depth, configured retention, and other schemas in the PostgreSQL database. Use <code>altspotter inspect --storage</code> and <code>altspotter inspect --growth</code> to view installation-specific measurements. The example configuration is not a claim about the size or capacity of a deployed host.

The retention manager drops eligible partitions in configured cleanup-priority order and respects each policy’s protected minimum. See [Retention](RETENTION.md) for the example horizons and partition layout.
