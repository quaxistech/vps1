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

## Units and sides (QUALITY-01/02)

| fields | unit | perspective |
|---|---|---|
| `expected_return`, `expected_mfe`, `expected_mae` | return **fraction** (0.009 = 0.9 %) | the LONG side; `expected_mae` ≤ 0 |
| `favorable_*_bps`, `adverse_*_bps`, `expected_up/down_excursion_bps`, `*_bps` | basis points | as named |

All conversions go through `src/engine/include/altstrategy/engine/units.hpp`.
Plan geometry 1 divided the fractions by 10 000 a second time, so a forecast
0.9 % excursion became a 0.0001 % target -- rounded onto the entry and nudged
one tick away -- and read the LONG excursions for both sides. Plan geometry 2
(`kPlanGeometryVersion`, recorded on every plan variant and proposal) takes
side-aware magnitudes in bps from the ACTIVE prediction only: a LONG reads up
as favourable and down as adverse, a SHORT the reverse; schema 3 per-side
values count only when all four are present and finite. A forecast without
magnitudes, a SHADOW one, or an unspecified side shapes no level.

Every price is put on the venue's tick at T0: the `instrument_spec_versions`
row in force at the context's event time (`valid_from <= T0`, the newest; the
higher version on a tie). Stops round away from the entry (ceil, in whole
ticks), targets to the nearest tick, never closer than one. `tick_source`
says which grid was used; a derived one adds the warning `TICK_SIZE_DERIVED`.
A target that does not cover the estimated round-trip cost adds
`PLAN_TARGET_BELOW_COST` (info) and is still published.

`ml_conviction` on a proposal is `p_side − p_opposite` of the econ estimate
(signed, [−1, 1]); the proto comment on tag 78 still says `|p_up − 0.5| · 2`
(see the Gateway's SIGNAL_CONTRACT.md).

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
