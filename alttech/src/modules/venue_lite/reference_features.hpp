#pragma once
// Frozen layout of the per-venue reference feature vector.
//
// The reference venues exist to give the cross-venue analytics a comparable
// view of each market, not to reproduce the primary venue's full indicator
// catalog. The set is therefore deliberately small and every member is either
// a standard indicator or a directly measured market quantity.
//
// The order is frozen: adding a metric is a new feature-set version.

#include <array>
#include <cstddef>
#include <string_view>

namespace alttech::venuelite {

enum RefFeature : std::size_t {
    RF_RETURN = 0,          // close/prev_close - 1
    RF_LOG_RETURN,          // ln(close/prev_close)
    RF_EMA_20,
    RF_EMA_50,
    RF_EMA_200,
    RF_EMA_ALIGNMENT,       // +1 bullish stacked, -1 bearish stacked, 0 mixed
    RF_EMA_SLOPE,           // one-bar change of EMA(20), in ATR units
    RF_MOMENTUM_10,         // close - close[-10]
    RF_ROC_10,              // close/close[-10] - 1
    RF_RSI_14,              // Wilder RSI
    RF_ATR_14,              // Wilder ATR
    RF_REALIZED_VOL_20,     // stddev of the last 20 log returns
    RF_BB_WIDTH_20,         // (upper - lower) / middle of Bollinger(20, 2)
    RF_BB_WIDTH_PERCENTILE, // empirical rank of bb_width in its own window
    RF_VWAP_RELATION,       // close/vwap - 1
    RF_VOLUME_ZSCORE_20,
    RF_TURNOVER_ZSCORE_20,
    RF_TRADE_IMBALANCE,     // (aggressive buy - sell) / total
    RF_SIGNED_VOLUME,       // signed volume of this bar, USDT notional
    RF_CVD,                 // cumulative signed volume over the window, USDT
    RF_TRADE_INTENSITY,     // trades per minute
    RF_SPREAD,
    RF_RELATIVE_SPREAD,
    RF_DEPTH_USDT,          // top-5 depth, USDT notional
    RF_BOOK_IMBALANCE,
    RF_MICROPRICE_REL,      // microprice/mid - 1
    RF_MARK_DEVIATION,      // mark/close - 1
    RF_INDEX_DEVIATION,     // index/close - 1
    RF_FUNDING_8H,          // funding rate normalized to an 8-hour interval
    RF_OI_CHANGE,           // relative open-interest change vs the previous minute
    RF_DATA_QUALITY,        // 1 valid, 0.5 degraded, 0 otherwise

    // --- BBO dynamics (set version 2) ----------------------------------------
    //
    // market_bbo_1m summarises the CONTINUOUS top-of-book stream, sampled 206
    // to 909 times per minute depending on venue — 28x to 125x denser than the
    // order-book snapshots the spread above is measured from (7.2/min). It is
    // the only source of what the spread did WITHIN the minute rather than
    // where it happened to be when a snapshot was taken.
    //
    // Measured on Bitget: mean intra-minute spread RANGE is 1.07 bps against a
    // mean spread of 2.49 — the spread moves by ~43% of its own mean inside a
    // single minute, entirely invisible to a 7-snapshot book.
    RF_BBO_SPREAD_MIN_BPS,   // tightest spread observed in the minute, bps
    RF_BBO_SPREAD_MAX_BPS,   // widest spread observed in the minute, bps
    RF_BBO_SPREAD_RANGE_BPS, // max - min: intra-minute spread volatility, bps
    RF_BBO_SPREAD_MEAN_BPS,  // (min+max)/2 as a mid estimate of the minute, bps
    RF_BBO_STABILITY,        // 1 - range/mean, clamped to [0,1]: how steady the quote was
    RF_BBO_MID_DRIFT,        // (bid_last+ask_last)/2 over (bid_first+ask_first)/2 - 1
    RF_BBO_SAMPLE_RATE,      // BBO updates per minute: quote activity

    // --- Intra-minute flow and liquidity dynamics (set version 3) -----------
    //
    // From market_trade_aggregates_1s and market_book_aggregates_1s, the only
    // stored series with second resolution. The microstructure module persists
    // 15s/30s/1m windows from the live stream, so nothing until now could see
    // WITHIN a 15-second window, and nothing at all could see it historically:
    // the live path leaves no record to replay.
    //
    // These are summary statistics of the second-level series inside the
    // minute, not a copy of it: the peak, the dispersion and the occupancy of
    // the minute, which is what distinguishes a burst from steady flow at the
    // same total volume.
    RF_FLOW_PEAK_TRADES_1S,   // busiest second's trade count in the minute
    RF_FLOW_PEAK_VOLUME_1S,   // busiest second's base volume
    RF_FLOW_BURST_RATIO,      // peak second's volume over the minute's mean second
    RF_FLOW_ACTIVE_SECONDS,   // fraction of the 60 seconds with any trade [0,1]
    RF_FLOW_SIGNED_DISPERSION,// stddev of per-second signed volume: flow choppiness
    RF_BOOK_DEPTH_MIN,        // shallowest depth observed in the minute (book snapshots)
    RF_BOOK_DEPTH_DROP,       // 1 - min/mean depth: liquidity withdrawal within the minute
    RF_FEATURE_COUNT,
};

inline constexpr std::string_view kReferenceSetName = "reference_lite";
inline constexpr std::string_view kReferenceMicroSetName = "reference_lite_micro";
// Version 2 appends the BBO dynamics block. Existing positions are unchanged,
// so a consumer reading by position keeps working and simply sees a longer
// vector.
inline constexpr int kReferenceSetVersion = 4;
inline constexpr std::string_view kReferenceEngine = "alttech-reference-1";

inline constexpr std::array<std::string_view, RF_FEATURE_COUNT> kReferenceFeatureNames = {
    "return", "log_return", "ema_20", "ema_50", "ema_200", "ema_alignment", "ema_slope",
    "momentum_10", "roc_10", "rsi_14", "atr_14", "realized_vol_20", "bb_width_20",
    "bb_width_percentile", "vwap_relation", "volume_zscore_20", "turnover_zscore_20",
    "trade_imbalance", "signed_volume", "cvd", "trade_intensity", "spread", "relative_spread",
    "depth_usdt", "book_imbalance", "microprice_rel", "mark_deviation", "index_deviation",
    "funding_8h", "oi_change", "data_quality",
    "bbo_spread_min_bps", "bbo_spread_max_bps", "bbo_spread_range_bps",
    "bbo_spread_mean_bps", "bbo_stability", "bbo_mid_drift", "bbo_sample_rate",
    "flow_peak_trades_1s", "flow_peak_volume_1s", "flow_burst_ratio",
    "flow_active_seconds", "flow_signed_dispersion", "book_depth_min",
    "book_depth_drop",
};

} // namespace alttech::venuelite
