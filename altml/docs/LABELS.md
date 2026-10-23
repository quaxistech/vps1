# Labels

Labels describe the **future** outcome of a closed Bitget 1m bar and are kept
strictly separate from features (features come from bars ≤ t; labels only from
bars > t). The catalog is frozen and versioned in `altml.label_set_versions`
(name `core-labels`, version 1); changing the label definitions requires a new
version, never an in-place edit of a frozen set.

Labels are written only from the **label store floor** onwards
(`labels::kLabelStoreDays`, 41 days: the 40-day retention target plus one;
`label_quality.hpp`). AltSpotter's candle history is older than every
`altml.labels` partition, so a new instrument starts at the floor, and the
one-time initial-ATR repair touches an instrument's first candle only when
the store can hold it — otherwise it completes with no rows (until
2026-10-23 it failed on 2026-02-01 every minute). `write_labels` creates the
months it writes into, so retention dropping a month the floor still reaches
cannot fail a write.

## Catalog

| Family | Labels | Horizons |
|---|---|---|
| return | `future_return_{1,3,5,10,15,30}m` — `log(close[t+h] / close[t])` | 1–30m |
| return after fees | `future_return_after_fees_{1,5,15,30}m` — minus round-trip taker fees | 1–30m |
| excursion | `max_favorable_excursion_{5,15,30}m`, `max_adverse_excursion_{5,15,30}m` | 5–30m |
| volatility | `future_volatility_{5,15,30}m` — stddev of 1m log returns | 5–30m |
| direction | `direction_{5,15,30}m` — sign of the return | 5–30m |
| barrier | `target_before_stop`, `time_to_target`, `time_to_stop` | 30m |

The barrier experiment: target = `close·(1 + k·ATRn)`, stop = `close·(1 -
k·ATRn)` with `k = 1.0`; the ATR proxy is a rolling mean true range (14)
computed **causally** (bars ≤ t) and is part of the label definition, not a
feature. If both barriers are hit inside one bar, the conservative rule
applies: stop is counted first (an ambiguous bar never resolves in the
model's favor).

## Leakage discipline

Label values use only bars strictly after t; the causal ATR warmup uses bars
≤ t. A bar is labeled only once its full horizon (30m ahead) has closed.
Fees are versioned via `altml.fee_models` — a label row records which fee
model it was computed with, so a model can never be trained against labels
built under different economics without an explicit version bump.

## Point-in-time correctness

The label builder anchors every computation to the candle's `revision`: a
label is only valid for the candle revision it was computed from, and the
periodic revision audit rewinds and recomputes labels whose source candle
was corrected after the fact (`src/altml/components/label_builder.cpp`).
