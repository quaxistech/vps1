# Strategy catalogue

Sixteen compiled-in hypotheses. Each row states the mechanism it claims, the
inputs it reads, the horizon it declares, and the lifecycle it is seeded at.

The catalogue is compiled, not configured: a strategy is a C++ class registered
at static-initialisation time, so the set that runs is exactly the set that was
built and tested. Lifecycle is the only per-deployment override, loaded from
`altstrategy.strategy_lifecycle` at startup.

## Why family matters

The ensemble measures agreement and *family independence*. Two strategies in one
family are expected to be right and wrong together, so a second opinion from the
same family is not a second piece of evidence. This is why the catalogue is
organised by mechanism rather than by indicator.

The original ten were not balanced this way: four trend + two breakout meant six
of ten were restatements of "the move continues", and their agreement read as
confirmation when it was mostly correlation. The six added hypotheses are each in
a family that had no member, and four of them read inputs (resting book,
executed tape, mark-index basis, higher grid) that no original strategy read.

| strategy | family | mechanism | primary inputs | horizon | seed |
|---|---|---|---|---|---|
| `momentum_continuation` | TREND | the move continues | `roc_*`, `adx_14`, `ema_*` | 3 h | ACTIVE |
| `trend_pullback` | TREND | pullback within trend resumes | `ema_20/50`, `atr_14` | 3 h | ACTIVE |
| `multi_timeframe_momentum_alignment` | TREND | aligned horizons persist | `mom_short/mid/long` | 3 h | CANDIDATE |
| `volume_weighted_momentum_burst` | TREND | flow-confirmed burst extends | `volume_surge`, `cvd_delta` | 90 m | CANDIDATE |
| `donchian_breakout_continuation` | BREAKOUT | range break trends | `donchian_*`, `atr_expansion` | 3 h | SHADOW |
| `ema_compression_expansion` | BREAKOUT | compression precedes expansion | `ema_compression` | 3 h | SHADOW |
| `range_extreme_mean_reversion` | MEAN_REVERSION | range boundary rejects | `rsi_14`, `adx_14`, zones | 1–3 h | RESEARCH |
| `funding_oi_crowding_unwind` | CROWDING | crowded positioning unwinds | `funding_*`, `oi_growth`, `price_stall` | 3–8 h | RESEARCH |
| `vwap_reclaim_continuation` | VWAP | reclaimed VWAP holds | `vwap_dev`, `vwap_lost_recently` | 3 h | SHADOW |
| `cross_venue_lead_lag_follow` | CROSS_VENUE | followers reprice to the leader | `lead_confidence`, `bitget_lag_ms` | 1–3 h | SHADOW |
| **`book_imbalance_pressure`** | MICROSTRUCTURE | price drifts to the heavier book side | `book_imbalance`, `book_microprice_dev` | 20 m | RESEARCH |
| **`cvd_absorption_reversal`** | ORDER_FLOW | an unconfirmed extreme is absorbed | `donchian_*`, `cvd_delta` | 45 m | RESEARCH |
| **`spread_dislocation_fade`** | LIQUIDITY | transient quote withdrawal reverts | `spread_bps`, depth, microprice | 15 m | RESEARCH |
| **`funding_basis_convergence`** | BASIS | mark converges to index as funding is paid | mark, index, `funding_rate_8h` | 8 h | RESEARCH |
| **`venue_dislocation_convergence`** | CROSS_VENUE | a non-leading venue reverts to consensus | `consensus_mid_price`, `lead_confidence` | 30 m | RESEARCH |
| **`htf_range_false_breakout_fade`** | MEAN_REVERSION | a break inside a higher range is a sweep | execution `donchian_*` + higher-grid `adx_14` | 4 h | RESEARCH |

All sixteen declare horizons far inside `kMaxHoldingMs` (48 h). That constant is
a refusal boundary, not a typical holding period: the longest declared horizon in
the catalogue is 8 hours, and a candidate declaring more than 48 h is **rejected**
rather than clamped, so a strategy cannot have its thesis silently rewritten.

## Explore vs exploit

A strategy seeded RESEARCH or SHADOW runs in `explore_shadow` selection mode: it
produces candidates, they are archived and scored, and they never carry an
execution recommendation. That is what the six new hypotheses are for right now —
they exist to accumulate outcomes, and none of them has an out-of-sample record.

## Metadata

Each strategy declares: `strategy_id/version`, `family`, supported sides +
holding classes, allowed/forbidden regimes, required features, required quality,
`max_candidate_freq_ms`, `cooldown_ms`, `min_history_bars`, an explanation
template, a bounded `parameter_schema` (name/type/min/max/step/default) and a
default lifecycle. Calibration may only move a parameter within its schema bound
on the `step` grid — never outside.

## Adding a strategy

Implement `IStrategy` in a `src/engine/strategies/*.cpp`, register it with
`ALTSTRATEGY_V2_REGISTER_STRATEGY`, seed it as `RESEARCH`/`SHADOW`, and let the
lifecycle coordinator promote it only after validation. The pure-logic tests in
`tests/unit/test_engine_strategies.cpp` cover LONG/SHORT/no-setup, eligibility
and determinism.
