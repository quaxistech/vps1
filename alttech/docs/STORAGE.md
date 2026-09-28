# Storage and retention

AltTech persists derived analytics in its own `alttech` database. The example
configuration defines a soft allocation, a shared database ceiling, and the
warning, cleanup, and critical ratios. Treat those values as deployment inputs;
size them for the database and retention policy in use.

## Retention policy

`alttech.retention_policies` is the authoritative per-table policy. Each row
defines its target retention, minimum retention, cleanup priority, and rationale.
Ordinary retention removes expired partitions at the target. Under budget
pressure, cleanup follows priority and does not reduce a table below its minimum.

Sub-minute microstructure windows are computed and published live. The example
configuration persists `15s`, `30s`, and `1m` windows; other windows can be
enabled through `modules.microstructure.persist_windows`.

## Maintenance and measurements

`alttech-maintenance.timer` keeps partitions available, applies retention, and
records storage measurements. The service does not derive growth from row
counts. `alttech-analytics storage-inspect` reports current table sizes and any
growth measurements available in `alttech.storage_budget_snapshots`.

For deployment limits and configuration keys, see [Configuration](CONFIGURATION.md).
