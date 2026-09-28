# Cold start and model state

The evaluator can process signals without a trained model. Model state affects
whether a diagnostic quality score is available; it does not change the
publication policy.

## Signal policy

`policy.cold_start` selects the action for a signal that passes contract,
freshness and holding-time validation. The setting retains its historic name;
it applies whether or not a diagnostic model is available:

| value | behaviour |
|---|---|
| `PASS_THROUGH_ALL` | Forward every signal that passes validation. This is the default. |
| `REJECT_ALL` | Reject otherwise-valid signals. Use for a deliberate service freeze. |

AltSignalQuality does not use the upstream execution recommendation or its own
quality score as a publication gate. The retired options
`PASS_THROUGH_RECOMMENDED`, `policy.require_upstream_recommendation: true` and a
non-zero `policy.min_quality_score` are rejected during configuration loading.

## Optional model lifecycle

Training is disabled by default. When enabled, the dataset component records
point-in-time examples from live evaluations and may backfill from
AltStrategy's archive. Labels are created only after outcomes mature. The
trainer performs walk-forward validation, calibration and configured promotion
checks before registering an approved model. A model can add a diagnostic
`quality_score` to an evaluation; that score does not change whether the valid
signal is forwarded.

The evaluation records the decision basis and whether a score was available.
When no usable model is loaded, the score and threshold are absent rather than
invented. A registered model is accepted only when its artifact checksum and
feature-schema identity match the running service. The decision basis remains
`COLD_START`; the score is not a live gate.

## Signal validation

Independent of model state, the evaluator checks the proposal contract, the
configured maximum signal age and the system holding-time ceiling. A malformed,
stale or over-limit signal is rejected and recorded with its reason.

See [FEATURE_CONTRACT.md](FEATURE_CONTRACT.md) for model inputs and
[CONFIGURATION.md](CONFIGURATION.md) for policy settings.
