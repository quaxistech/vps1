# Snapshot and archive retention

Database rows and exported training snapshots have separate retention policies.
The per-table policy in `altstrategy.retention_policies` is authoritative for
PostgreSQL data. Retention runs as part of the maintenance scheduler.

## Snapshot exports

Snapshots are immutable and content-addressed. A correction creates a new
incremental snapshot rather than changing an existing artifact. Each export
contains a manifest with its watermark and per-file checksums.

Export files live under `/var/lib/altstrategy/exports/`. The exporter removes
superseded files only when no retained snapshot or active reference depends on
them. See [Training snapshots](TRAINING_SNAPSHOTS.md) for layout and
[Database](DATABASE.md) for archive metadata tables.
