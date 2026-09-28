# Analytics

This document defines every metric AltTech produces and states where its
parameters come from. The rule the whole system follows: **a threshold is either
a textbook reading of a standard indicator, or it is expressed in units of the
market's own behaviour** (a percentile of its own history, a multiple of its own
ATR, a z-score of its own volume). No threshold depends on an instrument's price
level, tick size or notional scale, because such a threshold would silently mean
something different for BTC and for a small-cap alt.

## Primary-venue feature vector (`talib-core`)

The indicator mathematics is **TA-Lib 0.7.1** itself, driven through its
abstract interface. AltTech contributes no formula of its own here, which is
deliberate: TA-Lib is the reference implementation these indicators are usually
specified against, so a value can be checked against any other TA-Lib consumer.
The engine version is therefore the library version, `talib-0.7.1`.

### Coverage

TA-Lib 0.7.1 ships **161 functions**. AltTech binds **146** of them into
**995 feature columns**. Coverage is complete by construction — every function
is enumerated and bound — and the exclusions are named individually rather than
dropped by group, so a function a future TA-Lib release adds is included by
default instead of disappearing silently. A unit test asserts the whole list,
function by function.

How each function is expanded:

- functions with one time-period knob are evaluated across the period ladder
  **5, 9, 14, 20, 50, 100, 200** (clamped to each function's own valid range,
  duplicates dropped) — short, medium and long horizons rather than only the
  library default;
- functions exposing a moving-average type are evaluated for **every** flavour
  TA-Lib offers (SMA, EMA, WMA, DEMA, TEMA, TRIMA, KAMA, MAMA, T3). When a
  function has several MA knobs they are all set to the same flavour, which
  covers every type without a combinatorial explosion;
- functions taking a single real series are bound to `close`, and where a second
  binding is the standard reading they get a **twin**: `MAX`/`MAXINDEX` over
  **highs** and `MIN`/`MININDEX` over **lows** are the Donchian channel and the
  time since the extreme, and `SMA`/`STDDEV` over **volume** are what the volume
  z-score is made of. Without the twins those readings would not exist.

**The 15 exclusions** are element-wise transforms that do not provide a useful
market feature when applied directly to a price series:

| function | reason for exclusion |
|---|---|
| `ACOS`, `ASIN` | Their input domain is [-1,1], while prices are not bounded to that interval. |
| `COSH`, `SINH`, `EXP` | Their growth can overflow for ordinary price magnitudes. |
| `ATAN`, `TANH` | Saturating transforms of price do not add a useful independent signal. |
| `SIN`, `COS`, `TAN` | Trigonometric transforms of price have no market interpretation. |
| `CEIL`, `FLOOR` | Rounded prices duplicate the close series at coarser precision. |
| `LN`, `LOG10`, `SQRT` | Monotone transforms of close add no independent price information. |

These are element-wise math transforms, not indicators. Including them would add
columns that are permanently invalid, permanently constant, or exactly collinear
with an input — which makes the vector worse, not more complete.

The **Math Operators** group is *not* excluded: `MAX`, `MIN`, `MINMAX` and the
`*INDEX` variants over a period are Donchian bounds and time-since-extreme, and
`SUM` is a rolling sum — genuine rolling-window statistics, all finite on real
data.

The exact column list and its order live in the database:

```sql
SELECT position, output_name FROM alttech.feature_set_members m
JOIN alttech.feature_set_versions v ON v.id = m.feature_set_version_id
WHERE v.name = 'talib-core' ORDER BY position;
```

**Warm-up.** A bar is only published as `VALID` once every indicator in the set
has converged. The window is 2048 bars, above the longest TA-Lib lookback across
the period ladder (about 1194 bars at period 200). A gap in the source candles
resets the warm-up: an indicator must never become valid by accumulating
unrelated fragments across missing data.

**Validity.** A value that TA-Lib did not produce for a bar is marked invalid in
the bitmap. It is not zero and must not be read as zero.

### Flat projection

`venue_feature_projection` carries the handful of columns other modules and
ad-hoc SQL join on. Each is taken from the full vector **by catalog name**, never
by position:

| column | source |
|---|---|
| `ema_20`, `ema_50`, `ema_200` | `EMA_<n>_outReal` |
| `rsi_14` | `RSI_14_outReal` |
| `atr_14` | `ATR_14_outReal` |
| `adx_14` | `ADX_14_outReal` |
| `realized_vol_20` | `STDDEV_20_outReal` |
| `bb_width_20` | `(upper - lower) / middle` of `BBANDS_20_SMA` |
| `volume_zscore_20` | `(volume - SMA_VOL_20) / STDDEV_VOL_20` |
| `efficiency_ratio_20` | no TA-Lib equivalent — written `NULL` |

`bb_width_20` and `volume_zscore_20` are ratios of TA-Lib outputs, not new
indicator mathematics. A column with no counterpart is `NULL`, never filled with
a lookalike.

## Reference-venue feature vector (`reference_lite`)

Every venue also carries the full `talib-core` set. The lighter vector remains
useful alongside it, and both are produced for all four venues.

Two reasons it stays. First, most of it is *not* TA-Lib output and cannot be:
`spread`, `depth_usdt`, `book_imbalance`, `microprice_rel`, `funding_8h`,
`oi_change` and `trade_imbalance` are measured quantities, not indicators over
candles, so no TA-Lib catalog can contain them. Second, the cross-venue
consensus reads this vector by position (see `cross_venue_store.cpp`), which is
why the two sets share a partition but are never mixed: that query is pinned to
`engine_version = 'alttech-reference-1'`, because at the same array indices the
995-value layout holds entirely different features.

Thirty-one values, each either a standard indicator or a directly measured
quantity:

| group | members |
|---|---|
| returns | `return`, `log_return` |
| trend | `ema_20/50/200`, `ema_alignment`, `ema_slope` |
| momentum | `momentum_10`, `roc_10`, `rsi_14` |
| volatility | `atr_14`, `realized_vol_20`, `bb_width_20`, `bb_width_percentile` |
| activity | `vwap_relation`, `volume_zscore_20`, `turnover_zscore_20`, `trade_intensity` |
| order flow | `trade_imbalance`, `signed_volume`, `cvd` |
| liquidity | `spread`, `relative_spread`, `depth_usdt`, `book_imbalance`, `microprice_rel` |
| derivatives | `mark_deviation`, `index_deviation`, `funding_8h`, `oi_change` |
| quality | `data_quality` |

Definitions worth stating explicitly:

- **`rsi_14`** is Wilder's RSI: the average gain and loss are smoothed with
  Wilder's recursion (α = 1/period) seeded from the simple average of the first
  `period` changes. This matches TA-Lib, so the reference venues and the primary
  venue mean the same thing by "RSI".
- **`atr_14`** is Wilder's ATR, smoothed the same way.
- **`ema_slope`** is the one-bar change of EMA(20) **in ATR units**, so the same
  number means the same steepness on any instrument.
- **`bb_width_20`** is `(upper − lower) / middle` of a Bollinger(20, 2) band,
  i.e. `4σ / SMA₂₀`. σ is the population deviation, matching the band definition
  and TA-Lib's `STDDEV`.
- **`bb_width_percentile`** is the empirical rank of the current bandwidth
  within the bandwidth series of the same window. This, not the raw bandwidth,
  is what the regime classifier uses.
- **`cvd`** is the cumulative signed volume over the whole window, in USDT
  notional, built from the candles' own taker buy/sell split.
  **`signed_volume`** is the last bar only.
- **`funding_8h`** is the venue's funding rate normalized to an 8-hour interval.
  The interval is **measured** from that venue's own settlement history (the mean
  gap over its last eight settlements), never assumed: venues do not share an
  interval and a contract can change one. Without history the feature stays
  invalid rather than being computed from a guess.
- **`data_quality`** is 1 when altspotter reports `VALID` for that venue and
  instrument, 0.5 for `DEGRADED`, 0 otherwise.

## Microstructure

Trade and order-book features over tumbling 1s, 5s, 15s, 30s and 1m windows on
the primary venue, and a compact 1m set on the reference venues.

Trade side is the aggressor side as reported by the venue, so `trade_imbalance`
and `signed_volume` measure aggression, not net position change.

| feature | definition |
|---|---|
| `trade_count`, `trade_intensity` | count, and count per second |
| `aggressive_buy_volume`, `aggressive_sell_volume` | volume by aggressor side |
| `trade_imbalance` | `(buy − sell) / total` |
| `delta`, `signed_volume` | `buy − sell` for the window |
| `cvd` | running cumulative delta, checkpointed so it survives restart |
| `average_trade_size`, `median_trade_size`, `max_trade_size` | size distribution |
| `large_trade_intensity` | share of trades above **3× the window's own median size**, so the "large" threshold adapts to the instrument |
| `vwap`, `short_term_price_impact` | window VWAP and `(last − vwap) / vwap` |
| `volume_acceleration` | this window's volume over the previous window's |
| `burst_score` | trade count over an exponentially smoothed baseline (α = 0.1) |
| `book_spread_bps`, `book_imbalance`, `book_microprice_dev` | means over the window's book snapshots |
| `book_bid_depth`, `book_ask_depth`, `book_depth_ratio` | mean depth and its bid share |
| `book_mid_return`, `book_sample_count` | mid change across the window, and how many snapshots backed it |

`book_sample_count` is part of the vector on purpose: a book feature computed
from two snapshots is not the same evidence as one computed from fifty, and the
consumer can see which it got.

### Source, units and T0 semantics

| aspect | book features | trade features |
|---|---|---|
| **source** | `altspotter.public.market_orderbook_snapshots`, polled over `postgres_fdw` | `md.bitget.*.trade` live, and `market_trades_raw_*` |
| **sampling** | the venue-agnostic cadence: 5 s core / 15 s other, top-15 depth | every print |
| **window** | tumbling 1s / 5s / 15s / 30s / 1m, aligned to the clock | same |
| **T0** | `window_start`; a window is emitted only once closed, so nothing later than its own end can enter it | same |
| **units** | `book_spread_bps` bps of mid · `book_*_depth` base units summed over the stored levels · `book_imbalance`, `book_depth_ratio` dimensionless ratios · `book_microprice_dev` fraction of mid · `book_mid_return` fraction | volumes in base units, `trade_intensity` per second, `vwap` in quote units |
| **missing** | a window containing no book snapshot emits 0 for each book feature **and marks it invalid in the validity bitmap** (`book_sample_count` stays valid, at 0). The zero is flagged as absence, not published as a measurement. |

`LIMITATION` — with 15 s sampling against 15 s windows, a window frequently
contains no snapshot at all. A consumer that reads only the freshest window
therefore sees a spread of 0 most of the time. AltStrategy handles this by
keeping the **last observed** reading with its timestamp; see
`altstrategy/docs/DECISION_CONTEXT.md`.

### Formulas for the book features

Over the snapshots falling inside the window, with `b` = best bid, `a` = best
ask, `m = (a + b) / 2`, and depth summed over the stored top-15 levels:

```
book_spread_bps      mean( (a − b) / m × 10⁴ )
book_imbalance       mean( (bid_depth − ask_depth) / (bid_depth + ask_depth) )
book_depth_ratio     book_bid_depth / (book_bid_depth + book_ask_depth)
                     (the ratio OF THE MEANS, not the mean of the ratios)
book_bid_depth       mean( bid_depth )          book_ask_depth  mean( ask_depth )
book_microprice_dev  mean( (microprice − m) / m ),  microprice = (b·ask_sz + a·bid_sz) / (bid_sz + ask_sz)
book_mid_return      m_last / m_first − 1     (valid only when m_first > 0)
book_sample_count    number of snapshots in the window
```

### Order flow imbalance

`NOT IMPLEMENTED` — there is no OFI feature. `book_imbalance` is a **depth**
imbalance computed from the standing book at each sample; OFI is a different
quantity, derived from the *changes* between consecutive book states (additions
and cancellations at the touch). Computing it correctly needs the delta stream
rather than sampled snapshots.

The delta stream exists — AltSpotter publishes `md.<venue>.<symbol>.book.delta`
live — but it is not durable and is not currently consumed by AltTech. Adding
OFI would mean consuming those deltas, defining the aggregation window and T0
explicitly, and bumping the microstructure feature-set version.

### Downstream consumers

| consumer | uses | how |
|---|---|---|
| **AltStrategy** | `book_spread_bps` (position 18), `book_bid_depth`, `book_ask_depth` | positional decode into the `DecisionContext`; the spread drives cost model v2 |
| **AltML** | **none of it** | AltML trains on the 995-value TA-Lib set only; the microstructure vectors reach AltStrategy but not the model |

**What is persisted.** Every window is published live. Only the windows listed
in `modules.microstructure.persist_windows` are written to the database
(default 15s, 30s, 1m). Sub-15-second vectors dominate the storage budget by an
order of magnitude and are only actionable in real time, so keeping them out of
the database is a deliberate trade, not an omission — see
[STORAGE.md](STORAGE.md).

## Market structure

A zone is a price band that several independent detection methods agree on. The
methods, each contributing a confluence weight:

| method | weight | what it is |
|---|---|---|
| `pivot_cluster` | count of pivots | swing pivots (fractal, wing 2) over 500 bars, clustered at ATR-scaled distance |
| `volume_profile_poc` | 2.0 | point of control of the volume profile |
| `volume_profile_vah` / `_val` | 1.2 | bounds of the 70 % value area |
| `session_vwap` | 1.5 | VWAP of the current UTC day |
| `rolling_vwap` | 1.0 | VWAP of the last 100 bars |
| `prev_week_high` / `_low` | 1.6 | previous week's extremes |
| `prev_day_high` / `_low` | 1.4 | previous day's extremes |
| `donchian55_high` / `_low` | 1.2 | 55-bar channel bounds |
| `donchian20_high` / `_low` | 1.0 | 20-bar channel bounds |
| `round_number` | 0.8 | the 1-2-5 round-number grid nearest price |

The weights are an ordering, not a measurement: they encode that a weekly
extreme is a more widely watched level than a round number, and that the volume
point of control is the strongest single piece of evidence. They are the one
genuinely judgemental part of this module and are documented as such.

The volume profile is built from each bar's typical price `(H+L+C)/3` because
tick-level history is not retained. That approximation is stated here rather
than hidden: it spreads a bar's volume onto one price instead of across the
bar's range.

**Geometry**, all relative:

- band half-width `max(0.25·ATR, 5 bps of price)`;
- pivots merge into one cluster within `max(0.5·ATR, 10 bps)`;
- a break must clear the band by `0.5·ATR`;
- a rejection of at least `0.5·ATR` counts as a reaction.

**Strength** combines confluence, touches, proximity and volume:

```
strength = min(1, 0.15·Σweights + 0.4·min(1, touches/6) + 0.25·e^(−distance/10·ATR)
                 + 0.2·min(1, volume_weight/2))
```

**Lifecycle**: `CANDIDATE → ACTIVE → TESTED → WEAKENED → BROKEN | FLIPPED |
EXPIRED`. A broken zone creates the mirrored candidate at the same band — the
classic support/resistance role flip.

**The lifecycle is derived, never accumulated.** Touch and reaction counts, the
break verdict and the strength are recomputed from the trailing 120 bars on
every cycle; nothing is incremented from one cycle to the next. That is not a
stylistic choice: altspotter revises 5–8 % of closed bars, and the revisions
arrive with a 95th percentile around **seven hours** after the bar closed.
Anything accumulated bar by bar would drift away from the corrected data and
never come back — a `BROKEN` verdict taken on a close that was later repaired
would leave a zone that should not exist. Because the state is a function of the
window, a repaired candle simply produces the corrected answer on the next
cycle, and recomputing over unchanged data changes nothing.

A wick through the band is a **test**; only a *close* beyond it by half an ATR is
a **break**. A zone expires below strength 0.15; a new candidate needs 0.20. The
mirrored zone a break creates is guarded against duplication by band overlap, so
a re-derived break cannot spawn a new mirror on every cycle.

## Regimes

### Per-venue regime, primary venue (15m, 1h)

Four axes, all scale-free:

| axis | definition |
|---|---|
| `trend_strength` | `min(1, ADX(14)/50)`. Wilder's reading: below 20 no trend, 20–25 emerging, above 25 trending, above 50 very strong — hence 50 as the saturation point and 25 as the decision boundary. |
| `volatility_regime` | empirical percentile of Bollinger bandwidth over the **trailing 14 days** of the same instrument and timeframe, bounded by the classified bar so no future data leaks in |
| `volume_regime` | volume z-score over 20 bars, squashed to [0,1] with ±3σ saturation |
| `momentum_persistence` | Kaufman efficiency ratio over 20 bars: 0 pure noise, 1 perfectly directional |

Classification, in order:

1. `LOW_LIQUIDITY` — volume axis below 0.15 (about −2σ)
2. `CHAOTIC` — efficiency below 0.12 while volatility is above the 85th percentile
3. `TREND_UP` / `TREND_DOWN` — ADX ≥ 25 with price and EMA 20/50/200 stacked in the same direction
4. `BREAKOUT` — volatility above the 90th percentile confirmed by volume z > 1.5
5. `VOLATILITY_EXPANSION` — volatility above the 90th percentile without that confirmation
6. `VOLATILITY_COMPRESSION` — volatility below the 12th percentile
7. `RANGE` — the deterministic fallback
8. `UNDEFINED` — any classifier input missing

Every boundary is configurable under `modules.regime.thresholds`; the defaults
above are the conventional reading of each axis.

### Per-venue regime, all venues (1m)

The same idea in a lighter form: volatility is the bandwidth percentile within
the sweep window, and trend is the EMA(20)−EMA(200) spread **in ATR units** — a
spread of one ATR means the trend has carried price by a full unit of that
market's own noise. Expansion above the 90th percentile, breakout beyond 1.5 ATR
with expanded volatility, trend beyond 1 ATR, compression below the 10th
percentile, otherwise range.

The order is deliberate. Expansion is tested first because it is the most
actionable state and a breakout is simply an expansion that also has direction.
A clear trend is tested *before* compression, because a market that has moved by
more than an ATR of its own noise is trending even when its relative bandwidth
is low — labelling that "compression" would hide the very thing a consumer needs
to see. Compression is reserved for a market going nowhere, quietly.

### Global regime

See [CROSS_VENUE.md](CROSS_VENUE.md).

## What AltTech deliberately does not do

- It does not invent a value it could not compute. Every vector carries a
  validity bitmap, and every cross-venue record carries a quality status and the
  venue masks that explain it.
- It does not smooth across a data gap. A gap resets the warm-up.
- It does not average across venues without recording who contributed and with
  what weight — `cross_venue_components` makes every consensus reproducible.
- It does not fit or train anything. Every output is deterministic: the same
  inputs and the same version always give the same value.
