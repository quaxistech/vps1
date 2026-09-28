# Configuration

The example is [config/altsignalquality.example.yaml](../config/altsignalquality.example.yaml).
At runtime, the service reads `/etc/altsignalquality/altsignalquality.yaml`
and environment variables from its service environment. `${VARS}` are expanded
at load time; an unset variable is a configuration error.

## Sections

| section | controls |
|---|---|
| root | instance identity, logging, PostgreSQL and NATS connections, shutdown |
| `wiring` | input and output streams, subject prefixes, AltStrategy schema, model directory |
| `evaluator` | database pool, message batches, model/prior refresh, freshness, evaluation publication |
| `policy` | `PASS_THROUGH_ALL` or `REJECT_ALL` for otherwise-valid signals |
| `priors` | as-of historical outcome shrinkage and decay |
| `dataset` | priors/examples schedules, batch sizes, backfill and retention cadence |
| `training` | optional trainer, data window, temporal folds, model settings and promotion gates |
| `model_registry` | model-state polling |

## Signal policy

The default `policy.cold_start: PASS_THROUGH_ALL` forwards every signal that
passes contract, freshness and holding-time checks, whether or not a diagnostic
model is available. `REJECT_ALL` rejects otherwise-valid signals. Learned
model scores are diagnostic and do not gate publication. `training.enabled`
defaults to `false`.

The retired settings `policy.require_upstream_recommendation: true` and
`policy.min_quality_score` values other than zero are rejected. They must not be
used to make an economic recommendation or a model score a live gate.

## Optional training

Training settings apply only when `training.enabled` is true. The trainer
creates versioned artifacts after dataset eligibility, walk-forward validation,
calibration and the configured gates. See [TRAINING.md](TRAINING.md) and
[PROMOTION_GATES.md](PROMOTION_GATES.md).

## Validate

```sh
altsignalquality validate --config /etc/altsignalquality/altsignalquality.yaml
```

The command checks configuration, schema contracts, dataset invariants and
model compatibility.
