# Model artifacts

Every trained model is a directory under `/var/lib/altml/models/<model_id>/`,
written to a `.tmp-<model_id>` staging path and atomically renamed only after
every artifact is written — the trainer never leaves a partially written
directory where a loader could find it.

## Layout

```
/var/lib/altml/models/<model_id>/
├── model.txt               LightGBM text model
├── feature_names.json      ordered feature names (f_<name>)
├── sample_predictions.json rows + expected predictions for the write/read
│                          parity self-check and `altml model-validate`
└── report.json             gate metrics, walk-forward folds, hyperparameters
```

Model directories are immutable once published: a new training cycle always
writes a new `model_id`, never edits an existing one in place.

## Integrity

- **Content-addressed**: `altml.model_artifacts` records a SHA-256 checksum
  and size for every file; the inference component recomputes and compares
  the checksum before loading a model.
- **Registry paths are data, not authority**: the loader resolves symlinks
  and rejects any artifact path outside the configured `model_root` before
  ever opening it (`src/altml/components/inference.cpp`).
- **Feature-count check**: the loaded LightGBM booster's feature count must
  match the registry's recorded count, or the model is rejected.

## Retention

Retired/rejected/rolled-back model rows and their artifact directories are
pruned by the storage-guard component beyond
`storage.retired_model_keep_count`, keeping the currently `ACTIVE` model and a
bounded number of recent predecessors for rollback/audit. See
[STORAGE_BUDGET.md](STORAGE_BUDGET.md).

The loader reads each text artifact once into an owned buffer, verifies that
buffer, then loads it through LightGBM's string API. It never reopens the path
after verification. Empty, embedded-NUL, incomplete or larger-than-128-MiB text
artifacts are refused. Classifier and magnitude models retain the same root,
checksum, recipe, feature-name/width and shape checks. No model is retrained by
this integrity change.
