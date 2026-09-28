# Training Snapshots

The `training_snapshot_exporter` materializes mature training examples into
**immutable, content-addressed** snapshots for a batch consumer. It runs inside the single service via the `maintenance_scheduler` —
there is no separate exporter service or timer.

## Layout

```
/var/lib/altstrategy/exports/signal-quality/<snapshot-id>/
├── signals.<ext>       one row per mature example (id, pair, strategy, side, grade, times, sequence)
├── outcomes.<ext>      categorical label + net return + MFE/MAE + durations + binary_label_usable
├── contexts.<ext>      point-in-time context reference + feature manifest + quality
├── manifest.json       identity, watermark, counts, per-file sha256
└── checksums.sha256    sha256  filename  (one line per file)
```

`<ext>` is `parquet` where the build has `libarrow`/`libparquet`; otherwise a
validated CSV backend with identical schema, manifest and checksums (the format
is recorded in `manifest.json` and `signal_archive_snapshots.format`).

## Consistency

Each snapshot is read inside a single `REPEATABLE READ` transaction so signals
and their outcomes come from one committed DB state — never a signal from one
state with an outcome from another. Only committed, `dataset_eligible`, matured
examples are included; `AMBIGUOUS`/`DATA_INCOMPLETE` are excluded from the
binary-label set.

## FULL vs INCREMENTAL

- `FULL` — a consumer's first-boot bootstrap: every mature eligible example.
- `INCREMENTAL` — examples with `sequence > previous.to_sequence`.

The `sequence` column (a `BIGSERIAL` on `signal_training_examples`) is the
incremental watermark. `manifest.json` records `from_sequence`, `to_sequence`
and `previous_snapshot_id`.

## Manifest

`snapshot_id`, `kind`, `format`, `created_at_ms`, `cutoff_time_ms`,
`from_sequence`, `to_sequence`, `previous_snapshot_id`, per-table counts and the
per-file `{rows, bytes, sha256}` list. `manifest_hash` (sha256 of the manifest)
is stored in `signal_archive_snapshots` and published on
`archive.snapshot.created.v1`.

Implementation: `src/engine/io/snapshot_exporter.cpp` (testable columnar writer +
manifest + checksums in `tests/unit/test_io_assembler_exporter.cpp`).
