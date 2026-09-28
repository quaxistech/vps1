# AltML to AltStrategy prediction contract

AltML publishes `PredictionEnvelope` messages containing `PredictionV1` on
`prediction.bitget.<symbol>.<timeframe>.v1`. AltStrategy consumes these messages
asynchronously; there is no request/response path.

The current `prediction_schema_version` is 4. AltStrategy accepts schema 3
while a producer is upgraded. The envelope protobuf schema version is separate
from the prediction schema version.

## Meaning

A prediction describes a model's forecast for a market horizon. It is not an
order, position size, leverage, or execution approval. `probability_calibrated`
indicates whether the forecast passed AltML's holdout calibration test. When
false, the scores must not be interpreted as calibrated probabilities.

The cost and expected-net fields are diagnostics. AltStrategy records them with
the proposal but does not use them as a publication gate.

## Prediction fields

Field numbers refer to `proto/altml_forecast.proto`.

| Field number(s) | Fields | Meaning |
|---|---|---|
| 1–7 | `prediction_id`, `timeframe`, `bar_open_time_ms`, `bar_revision`, `model_id`, `model_version`, `feature_set_version` | Identity, target interval, and model/feature lineage |
| 8–9 | `probability_up`, `probability_down` | Compatibility probability fields |
| 10–14 | `expected_return`, `probability_target_before_stop`, `expected_mfe`, `expected_mae`, `expected_holding_time_min` | Optional compatibility estimates; fields without a producer value remain unset |
| 15–19 | `confidence`, `quality_status`, `computed_at_ms`, `latency_us`, `prediction_schema_version` | Score metadata and freshness |
| 20–21 | `target_venue`, `abstain` | Forecast target and abstention state |
| 22–32 | `horizon_minutes`, `objective`, `probability_long`, `probability_neutral`, `probability_short`, `expected_net_return_long_bps`, `expected_net_return_short_bps`, `round_trip_cost_bps`, `calibrator`, `recipe_version`, `aligned_missing` | Horizon-specific forecast, economic diagnostics, and input availability |
| 33–43 | `cost_model_version`, `fee_bps`, `spread_bps`, `impact_bps`, `spread_observed`, `policy_margin_bps`, `policy_version`, `favorable_long_bps`, `adverse_long_bps`, `favorable_short_bps`, `adverse_short_bps` | Cost provenance, policy metadata, and side-specific magnitudes |
| 44–50 | `probability_calibrated`, `label_definition`, `expected_up_excursion_bps`, `expected_down_excursion_bps`, `magnitude_source`, `calibration_version`, `model_role` | Calibration verdict, target meaning, excursions, and model role |

Optional protobuf fields absent from a message are unavailable. Consumers must
not replace absence with zero.

## Market-prediction semantics

`model_role` is `ACTIVE` or `SHADOW`. `magnitude_source` states whether
excursions come from class means or a regression model. `label_definition`
identifies the target being forecast. `aligned_missing` counts missing aligned
input blocks; it is not a forecast value.

AltStrategy carries forecasts for each served horizon on a candidate. A missing,
stale, abstaining, incompatible, or incomplete forecast is represented by its
condition and may affect the candidate's quality grade. It does not determine
whether an otherwise valid market setup is published.

An economic diagnostic may be `NO_DATA`, `MODEL_UNAVAILABLE`, `MODEL_STALE`,
`MODEL_ABSTAIN`, `MODEL_SHADOW`, `HORIZON_CONFLICT`, `NO_EDGE`, `SMALL_EDGE`,
`POSITIVE_EDGE`, or `STRONG_EDGE`. These states and `ECON_DIAG_*` warnings do not
make trading decisions.

## Freshness and point-in-time use

A forecast is usable at decision time T0 only when it was computed no later than
T0 and its target bar is within the configured freshness bound:

```text
computed_at_ms <= T0
T0 - bar_close(bar_open_time_ms, timeframe) <= max_prediction_age
```

AltStrategy does not substitute a later forecast or a value from another venue
or timeframe. The vendored protobuf and AltML's producer definition are the
wire-level sources of truth.
