# Promotion gates

AltML promotes forecasts based on out-of-sample market prediction quality.
Economic results accompany each report as diagnostics; they do not determine
model promotion or execution.

## Market-quality gates

For `mode: econ` (`market-1`), a candidate must satisfy the configured
walk-forward and holdout requirements:

- mean walk-forward log-loss skill above zero, with at least
  `training.min_positive_folds` positive folds;
- positive holdout log-loss and Brier skill against the training base rates;
- where a comparable ACTIVE model exists, stronger holdout log-loss skill on
  the same rows;
- saved-artifact parity with the in-memory calibrated probabilities;
- compatibility between the registered model feature space and its feature
  recipe.

The report also records economic diagnostics and the holdout calibration test.
The calibration verdict is carried with predictions as
`probability_calibrated`; it describes calibration quality, not net return.
Models configured for shadow serving are published with `model_role = SHADOW`.

## Validation method

Walk-forward splits group rows by timestamp and expand the training window. A
purged embargo of at least the maximum label horizon prevents label windows
from crossing fold boundaries. The holdout remains separate from training and
validation.

## Model lifecycle

```text
CANDIDATE → REJECTED
CANDIDATE → APPROVED   (all gates pass; automatic promotion disabled)
CANDIDATE → ACTIVE     (all gates pass; automatic promotion enabled)
ACTIVE   → RETIRED     (replaced by a new ACTIVE model)
```

There is at most one ACTIVE model per objective. The transactional transition is
recorded in `altml.model_events`. Model rows record the objective, label and
feature definitions, dataset fingerprint, split windows, per-fold and holdout
metrics, calibration and cost-model versions, and artifact checksum.
