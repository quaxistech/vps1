# Strategy hypotheses and sources

The catalogue contains falsifiable hypotheses, not claims of profitability.
Published research motivates some hypotheses but does not establish their
performance on crypto perpetuals or on AltStrategy's horizons.

## Trend and breakout

- `momentum_continuation`, `trend_pullback`,
  `multi_timeframe_momentum_alignment`, and
  `volume_weighted_momentum_burst` build on cross-sectional and time-series
  momentum (Jegadeesh & Titman, 1993; Moskowitz, Ooi & Pedersen, 2012). Their
  short-horizon crypto application is a hypothesis. Falsify it when continuation
  is no better than the unconditional baseline under the current cost model.
- `donchian_breakout_continuation` and `ema_compression_expansion` use channel
  breaks and volatility persistence (Engle, 1982; Bollerslev, 1986). Falsify
  them when breaks revert within the declared horizon at or above the baseline
  rate.

## Mean reversion and crowding

- `range_extreme_mean_reversion` hypothesizes rejection at the boundary of a
  measured range, consistent with inventory-driven market-making models such as
  Ho & Stoll (1981).
- `funding_oi_crowding_unwind` hypothesizes that rising open interest combined
  with flat price and extreme funding marks crowded positioning that can
  unwind. Its falsifier is persistence through the declared horizon.
- `htf_range_false_breakout_fade` hypothesizes that a lower-timeframe break
  inside a higher-timeframe range reverts into that range. It requires both
  timeframes to be present; absent higher-timeframe context is not substituted.

## VWAP and cross-venue

- `vwap_reclaim_continuation` treats VWAP as an execution benchmark and tests
  whether a reclaim holds. The falsifier is reclaims failing to outperform the
  unconditional baseline.
- `cross_venue_lead_lag_follow` tests whether a venue's repricing follows the
  venue leading price discovery (Hasbrouck, 1995; Gonzalo & Granger, 1995).
- `venue_dislocation_convergence` tests whether a non-leading venue's deviation
  from cross-venue consensus closes.

## Microstructure and flow

- `book_imbalance_pressure` tests whether price drifts toward the heavier side
  of resting depth. Cont, Kukanov & Stoikov (2014) study order-flow imbalance;
  standing depth is a related but weaker proxy.
- `cvd_absorption_reversal` tests whether an extreme without confirming signed
  flow is absorbed and reverts, drawing on price-impact models such as Kyle
  (1985).
- `spread_dislocation_fade` tests whether spread widening with intact depth
  represents transient quote withdrawal rather than repricing. It requires
  measured spread and depth.

## Funding and basis

- `funding_basis_convergence` tests whether a mark/index basis combined with
  directional funding converges around settlement. Its declared horizon is
  tied to the funding interval.

Each hypothesis is defined operationally in
[Strategy catalogue](STRATEGY_CATALOGUE.md). Performance evidence comes from
separate replay and walk-forward runs and must identify its time range,
population, and cost assumptions.
