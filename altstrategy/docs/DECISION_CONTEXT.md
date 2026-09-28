# Decision context

`DecisionContext` (`altstrategy.v1.DecisionContext`, C++
`engine::DecisionContext`) is the atomic, versioned, point-in-time input to the
strategy catalogue. The context assembler combines AltTech analytics,
AltSpotter universe state, and AltML forecasts after checking source revision,
freshness, and availability. It does not join unrelated stale messages.

Every value is used with its source timestamp and version. Missing, stale, or
non-finite values remain explicit; they are not replaced with values from a
different venue, timeframe, or later observation.

## Identity & time (point-in-time correct)

`context_id`, `canonical_pair_id`, `bitget_instrument_id`, `event_time_ms`
(observation time), `available_at_ms`, `expires_at_ms`, `context_revision`,
`feature_manifest_hash`, `producer_versions`. The full snapshot is persisted to
`altstrategy.decision_context_snapshots` and never overwritten with recomputed
values without a new `context_revision`.

## Feature vocabulary

AltTech ships **compact ordered** feature vectors; the assembler decodes index
`i` → `values[i]` using the ordered name lists in `context:` config. The
canonical names the strategies consume:

**Bitget features** (`bitget_features`): `close open high low volume ema_20
ema_50 ema_200 rsi_14 atr_14 adx_14 bb_width_20 volume_zscore_20 vwap vwap_dev
mom_1m mom_5m mom_15m donchian_high_20 donchian_low_20 ema_compression
atr_expansion spread_bps top_depth_usdt`.

**Cross-venue features** (`cross_venue_features`): `momentum_agreement
lead_confidence bitget_lag dislocation funding_divergence oi_growth
breakout_agreement atr_14 liquidity_rank expected_slippage_bps funding_rate
spread_bps`.

**Microstructure** (`microstructure`): `trade_imbalance cvd_delta burst_score
turnover_acceleration volume_zscore trade_intensity large_trade_intensity`.

A feature value that is NaN/Inf is dropped (never read as data). Adjust the name
lists in `altstrategy.yaml` to match the AltTech feature-set order for the scope.

## Regimes, structure, forecast, costs

`bitget_regime`, `global_regime`, `regime_divergence`; `bitget_projected_zones`
(support/resistance/dynamic); `market_forecast` (direction, net-return quantiles,
MFE/MAE quantiles, volatility, uncertainty, OOD, abstain, quality route); and the
cost/liquidity context (`fees_bps funding_rate mark_price spread_bps
top_depth_usdt liquidity_rank expected_slippage_bps`), plus two flags that say
whether the market values are **measurements or stand-ins**:
`spread_observed` and `depth_observed`.

### Observed versus assumed

The spread comes from the AltTech microstructure vector and is selected from
the latest observation at or before T0, subject to
`max_microstructure_age_sec`. When no eligible observation exists, the configured
fallback is used and `spread_observed` is false. The observation flag makes the
measured value distinguishable from an assumption.

### Cost

`fees_bps` is a configured per-side default and **no longer determines cost**.
Cost model v2 (`engine/cost_model.hpp`) charges per leg from `altml.fee_models`:
maker 2 bps, taker 6 bps; half the spread for each **taker** leg only; and the
depth-impact term with the spread removed, because `expected_slippage_bps`
already contains half the spread. The decomposition is persisted on the proposal
(`cost_model_version`, `fee_bps_round_trip`, `spread_cost_bps`,
`slippage_cost_bps`, `round_trip_cost_bps`, `spread_observed`) so the trade is
scored with the cost it was planned with. See
[MARKET_OUTCOMES.md](MARKET_OUTCOMES.md).

## Quality

`data_quality` ∈ {STRICT_4_OF_4, DEGRADED_3_OF_4, DEGRADED_2_OF_4, INVALID}
mirrors the AltTech cross-venue quality. A strategy declares a minimum required
quality; a context below it — or INVALID — makes the strategy ineligible.

## Units

Every derived quantity states its unit at the point it is computed in
`derive_engine_features` (`context_assembler.cpp`). `vwap_dev` is measured in
basis points; `atr_expansion` is a ratio; `ema_compression` is a fraction of
price; momentum values are percentages; and `volume_surge` is a log-ratio.


A composite whose inputs are missing is **absent**, not zero — the strategy's own
default then applies, and the strategy can tell the difference.

## Observing the multi-timeframe context

The higher grids are invisible from outside until a decision carries one, so the
assembler logs `context_grid_received` the first time each pair+grid arrives
(with `valid` and `values`), and `context_grid_skipped` with a reason
(`not_valid`, `no_values`, `not_yet_closed`, `stale`) when a held grid does not
enter a decision. Without these, "no grids in any context" is
indistinguishable from "the vectors never arrived".
