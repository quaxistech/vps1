# Dataset and training lifecycle

Training is optional and disabled by default. When enabled, it builds a
diagnostic signal-quality model. The resulting score is recorded with
evaluations but does not gate live publication.

## Dataset

The dataset component maintains as-of outcome priors and immutable training
examples:

- **Live examples** reuse the feature vector saved when the proposal was
  evaluated.
- **Backfilled examples** are reconstructed through the same assembler from
  AltStrategy's proposals, contexts and outcome archive.
- **Maturation**: an example is eligible only after its outcome label is
  knowable. Priors only include outcomes mature at the proposal timestamp.

Rows are keyed by proposal, feature-schema version and label-spec version.
Changing either contract creates a new row set and preserves existing model
lineage. Persistent watermarks make each materialization pass resumable.

Run one dataset pass with:

```sh
altsignalquality dataset-build --config /etc/altsignalquality/altsignalquality.yaml
```

## Training cycle

When enabled, the trainer:

1. Loads eligible examples within the configured time window and row/memory
   limits.
2. Audits feature and label timing, then creates expanding walk-forward folds
   with label-maturation purges and an embargo.
3. Fits a LightGBM classifier and calibrates its probability output on the
   designated validation data.
4. Evaluates ranking, calibration, outcome metrics and stability on held-out
   data. A selected threshold is used only for model evaluation and promotion
   reports; it is not applied to live publication.
5. Writes a versioned model artifact and registers its dataset, schema,
   calibration and metrics. Promotion follows
   [PROMOTION_GATES.md](PROMOTION_GATES.md).

The trainer records insufficient data as a deferred cycle. It does not modify
the live signal policy when training is unavailable or a model is rejected.

## Determinism and scheduling

Training parameters, schedule and resource limits are controlled by the
`training` section of the example configuration. Artifacts record the dataset
version, feature-schema identity, calibration and evaluation report. The
trainer yields to the evaluator under configured resource pressure.

Run one cycle on demand with:

```sh
altsignalquality train --config /etc/altsignalquality/altsignalquality.yaml
```

See [FEATURE_CONTRACT.md](FEATURE_CONTRACT.md) for model inputs and
[COLD_START.md](COLD_START.md) for how model state relates to live decisions.
