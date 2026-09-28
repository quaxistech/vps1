# Cross-venue analytics

The cross-venue module answers one question for every closed minute of every
eligible pair: **what do the four venues, taken together, say about this
market — and how much should you believe them?**

## 1. Event-time alignment

The four venues close the same minute at slightly different wall-clock times and
deliver it with different lag. Aligning on arrival time would silently compare
different minutes, so alignment is done on **event time**: the canonical bar
window `[open, open+60s)`, with each venue's bar matched to the window it
belongs to.

A window is evaluated once its **watermark** has elapsed
(`cross_venue.alignment.bar_watermark_timeout_ms`, default 4 s after close),
which bounds how long the module waits for a slow venue.

| status | meaning |
|---|---|
| `STRICT_ALIGNED` | all four venues present and within the skew bound |
| `DEGRADED_ALIGNED` | the primary venue plus at least one reference, after the watermark |
| `LATE_REVISION` | a venue or a revision arrived after the window was emitted |
| `INVALID` | the primary venue is missing after the watermark |
| `WAITING` | the watermark has not elapsed yet |

Without the primary venue the window is never usable: every cross-venue output
exists to inform trading on Bitget, and a consensus that excludes the venue you
trade on describes a market you are not in.

**Skew is a property of the venues' clocks, not of the bar.** It is the spread
between the participating venues' clock offsets, taken from altspotter's
`exchange_clock_offsets` estimates. Two venues whose clocks agree describe the
same instant however long ago the bar closed. The observed value is stored in
`venue_alignment_state.max_cross_venue_skew_ms`, so the bound can be set from
measurement rather than guessed.

## 2. Quality

Quality is a property of the window, not of the estimate, and it is recorded
next to every value:

| status | condition |
|---|---|
| `STRICT_VALID` | primary venue valid, all three references usable, skew within bound |
| `DEGRADED_3_OF_4` | primary venue valid, two references usable |
| `DEGRADED_2_OF_4` | primary venue valid, one reference usable |
| `INVALID` | primary venue unusable, or no reference at all |

A venue is usable when altspotter reports it `VALID` or `DEGRADED` and its own
market data is no further behind than `max_venue_data_age_ms`. That age is a
live health signal about the venue, measured by altspotter — **not** the age of
the bar being computed. Analytics run over closed bars read from durable
storage, so how long ago a bar closed says nothing about whether a venue is
healthy, and must not enter the decision.

`quality_reasons` records why each venue was excluded — `okx:data_too_old`,
`bybit:missing`, `skew_exceeded` — so a degraded window can be explained rather
than merely flagged.

`confidence` is the share of venues that participated, reduced when the skew
bound was exceeded.

## 3. Consensus

Each venue enters the estimator with a weight that reflects how much its price
deserves to count:

```
weight = historical_reliability
       × quality_factor                       (VALID 1, DEGRADED lower, else 0)
       × scale/(scale + venue_data_age)       freshness
       × scale/(scale + |clock_offset|)       clock trust
       × scale/(scale + relative_spread)      tighter spread counts more
       × depth/(depth + reference_depth)      deeper book counts more
```

Each factor is a bounded rational decay in [0,1] that equals ½ at its scale
parameter, so no single factor can dominate and no weight can go negative. A
metric a venue did not report is treated neutrally (factor 1) rather than
punished to zero — absence of evidence is not evidence of a bad venue.

Three estimators are available; the default is the **quality-weighted median**,
because a median is not moved by one venue printing an outlier, which is exactly
the failure mode a multi-venue consensus exists to survive.

| method | when to prefer it |
|---|---|
| `quality_weighted_median` | default; robust to a single bad venue |
| `quality_weighted_mean` | when you want every venue to move the estimate |
| `trimmed_weighted_mean` | a compromise: trims the weight tails, averages the middle |

Every venue's weight, value, inclusion and exclusion reason is written to
`cross_venue_components`, so any published consensus can be recomputed exactly
from stored data.

## 4. What is computed

| group | metrics |
|---|---|
| price | consensus mid, mark and index; each venue's deviation in bps; the maximum deviation; weighted dispersion in bps |
| momentum | consensus log return, share of venues agreeing on its sign, dispersion of returns, the primary venue's divergence from it |
| order flow | cross-venue signed volume in USDT, agreement on its sign, dispersion |
| funding | consensus 8-hour-equivalent funding, dispersion, primary-venue deviation |
| open interest | consensus relative change, agreement on direction, primary-venue deviation |
| liquidity | dispersion of relative spreads, and the primary venue's liquidity rank (1 = tightest) |
| lead-lag | per-venue lead score, and the primary venue's lag to the consensus in ms |

Deviations are in **basis points** so they are comparable across instruments.
Funding is normalized to an 8-hour interval using each venue's **measured**
settlement interval, taken from that venue's own settlement history — venues do
not share an interval, and assuming one would make the comparison meaningless.

## 5. Lead-lag

For each venue pair, the cross-correlation of their 1-minute log returns is
computed over lags up to `lead_lag_max_lag` and the peak is taken. The lag at
the peak is the apparent information flow between the two venues.

**Clock skew is corrected before the lag is believed.** An apparent lag equal to
the two venues' relative clock offset is a measurement artefact, not information
flow, so the measured lag is corrected by `(offset_i − offset_j) / interval`,
which cancels a purely clock-induced lag to zero.

A pair contributes only when the peak correlation exceeds `min_abs_corr`;
below that the "lag" is fitted noise. Each venue's lead score is its
correlation-weighted mean corrected lag across all pairs it appears in, so a
venue that consistently moves first across several counterparties scores higher
than one that led once by chance.

## 6. Global regime

The four per-venue 1-minute regimes are combined into one:

- **the winner** is the regime carrying the most weight, using the *same*
  consensus weights as the price estimate — so one venue's quality has identical
  influence on every cross-venue product of that window. Ties break on the
  regime name, so the result is deterministic;
- **`venue_agreement`** is the winner's share of the participating weight;
- **`venue_dispersion`** is the normalized Shannon entropy of the weighted
  regime distribution: 0 when the venues are unanimous, 1 when maximally split.
  Entropy is the standard dispersion measure for a categorical distribution, and
  unlike "count of distinct answers" it reflects *how* split the venues are;
- **`trend_consensus`, `volatility_consensus`, `liquidity_consensus`** are the
  weighted means of the per-venue axes;
- **`bitget_regime_divergence`** is set when the primary venue disagrees with the
  consensus — a directly tradable signal that the venue you trade on is behaving
  differently from the market as a whole;
- **`regime_confidence`** is agreement multiplied by the weighted mean per-venue
  confidence, so both "the venues agree" and "each venue was sure" must hold.

Venues reporting `UNDEFINED` do not vote. If no venue has a defined regime, no
global regime is written — an absent row is honest, a fabricated `UNDEFINED`
consensus is not.

Transitions are recorded in `regime_events` with scope `CROSS_VENUE`.

## 7. Consensus structure zones

Venue prices are not directly comparable: each venue has its own basis. A zone
is therefore normalized into that venue's own frame before anything is compared:

```
normalized_center = zone_center / venue_mid − 1
```

which is the zone's distance from that venue's mid price, in relative terms.
This removes the basis and requires no volatility estimate.

**Clustering is parameter-free**: two normalized zones belong to the same
cluster when their **bands overlap**. That is a geometric statement — once the
basis is removed, the venues are literally pointing at the same price region —
rather than a tuned distance threshold. Support and resistance never merge.

A cluster becomes a consensus zone when at least `min_consensus_venues` (default
2) distinct venues support it; it is `ACTIVE` at three or more venues and
`CANDIDATE` at two. Its strength is the weighted mean of the member zones'
strengths, and its confidence combines venue count with that strength.

An existing consensus zone whose band still overlaps a new cluster is **updated**
rather than replaced, so a level that drifts with the market keeps its identity
and its lifecycle. A zone no venue has confirmed for `zone_stale_after_sec`
(default 30 min) expires.

## 8. Projection onto the primary venue

A normalized zone is not tradable — you trade at Bitget's prices. The projection
maps it back:

```
projected_center = bitget_mid × (1 + normalized_center)
basis_adjustment = bitget_mid − consensus_mid
projection_quality = zone_confidence × window_confidence
```

`basis_adjustment` records how far the primary venue sat from the consensus when
the projection was made, so a consumer can see the basis that was corrected for.
`projection_quality` folds in the window's own confidence: a level derived from
a degraded window is projected with correspondingly lower quality rather than
presented as equally trustworthy.

## 9. Degradation, not failure

A reference venue going away degrades the quality status and lowers confidence.
It never stops the module, never blocks the primary venue's own analytics, and
never causes a silent substitution. The consumer sees exactly which venues
contributed through `venue_mask`, `missing_venue_mask`, `stale_venue_mask` and
`desynced_venue_mask`, and can decide for itself whether a `DEGRADED_3_OF_4`
window is good enough for what it is doing.
