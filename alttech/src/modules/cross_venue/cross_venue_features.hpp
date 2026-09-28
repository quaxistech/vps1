#pragma once
// Fixed ordered value layout for the cross_venue_core feature set (prompt
//). The order is frozen and mirrored in the DB feature-set
// members; adding metrics is a new feature-set version, never a reorder.

#include <array>
#include <cstddef>
#include <string_view>

namespace alttech::crossvenue {

enum XvFeature : std::size_t {
    XV_CONSENSUS_MID_PRICE = 0,
    XV_CONSENSUS_MARK_PRICE,
    XV_CONSENSUS_INDEX_PRICE,
    XV_BITGET_MID_DEVIATION_BPS,
    XV_BINANCE_MID_DEVIATION_BPS,
    XV_BYBIT_MID_DEVIATION_BPS,
    XV_OKX_MID_DEVIATION_BPS,
    XV_MAX_VENUE_PRICE_DEVIATION_BPS,
    XV_VENUE_PRICE_DISPERSION_BPS,
    XV_VENUE_RETURN_DISPERSION,
    XV_CONSENSUS_MOMENTUM_SCORE,
    XV_VENUE_MOMENTUM_AGREEMENT,
    XV_BITGET_MOMENTUM_DIVERGENCE,
    XV_CROSS_VENUE_SIGNED_VOLUME,
    XV_ORDERFLOW_AGREEMENT,
    XV_ORDERFLOW_DISPERSION,
    XV_CONSENSUS_FUNDING_8H,
    XV_FUNDING_DISPERSION,
    XV_BITGET_FUNDING_DEVIATION,
    XV_CONSENSUS_OI_CHANGE,
    XV_OI_DIRECTION_AGREEMENT,
    XV_BITGET_OI_DEVIATION,
    XV_CROSS_VENUE_SPREAD_DISPERSION,
    XV_BITGET_LIQUIDITY_RANK,
    XV_BINANCE_LEAD_SCORE,
    XV_BYBIT_LEAD_SCORE,
    XV_OKX_LEAD_SCORE,
    XV_BITGET_LEAD_SCORE,
    XV_BITGET_LAG_TO_CONSENSUS_MS,
    XV_CONFIDENCE,
    // --- v2: cross-venue TECHNICAL agreement -------------------------------
    // Appended, never inserted: positions 0..29 keep their meaning so an
    // existing consumer's position map stays correct across the version bump.
    //
    // Until v2 the four-venue view agreed on price, order flow and funding but
    // said nothing about whether the venues agreed on the technical picture.
    // Each venue already has its indicators computed (the projection for the
    // primary venue, reference_lite for the others) and they were being written
    // and read by nobody. These turn that into confirmation: an overbought
    // reading on one venue is noise, the same reading on four is a state.
    XV_CONSENSUS_RSI_14 = 30,
    XV_RSI_DISPERSION,           // stddev across venues; low = they agree
    XV_BITGET_RSI_DEVIATION,     // primary venue minus consensus
    XV_CONSENSUS_REALIZED_VOL,
    XV_VOLATILITY_AGREEMENT,     // 1 = identical realized vol, 0 = wildly apart
    XV_TREND_ALIGNMENT_AGREEMENT,// mean of per-venue EMA stacking, -1..+1
    XV_BITGET_TREND_DIVERGENCE,  // primary venue's stacking minus the mean
    XV_FEATURE_COUNT,
};

inline constexpr std::string_view kCrossVenueSetName = "cross_venue_core";
inline constexpr int kCrossVenueSetVersion = 2;
inline constexpr std::string_view kCrossVenueMethodVersion = "xv-1.0.0";

inline constexpr std::array<std::string_view, XV_FEATURE_COUNT> kCrossVenueFeatureNames = {
    "consensus_mid_price",
    "consensus_mark_price",
    "consensus_index_price",
    "bitget_mid_deviation_bps",
    "binance_mid_deviation_bps",
    "bybit_mid_deviation_bps",
    "okx_mid_deviation_bps",
    "max_venue_price_deviation_bps",
    "venue_price_dispersion_bps",
    "venue_return_dispersion",
    "consensus_momentum_score",
    "venue_momentum_agreement",
    "bitget_momentum_divergence",
    "cross_venue_signed_volume",
    "orderflow_agreement",
    "orderflow_dispersion",
    "consensus_funding_8h",
    "funding_dispersion",
    "bitget_funding_deviation",
    "consensus_oi_change",
    "oi_direction_agreement",
    "bitget_oi_deviation",
    "cross_venue_spread_dispersion",
    "bitget_liquidity_rank",
    "binance_lead_score",
    "bybit_lead_score",
    "okx_lead_score",
    "bitget_lead_score",
    "bitget_lag_to_consensus_ms",
    "confidence",
    "consensus_rsi_14",
    "rsi_dispersion",
    "bitget_rsi_deviation",
    "consensus_realized_vol",
    "volatility_agreement",
    "trend_alignment_agreement",
    "bitget_trend_divergence",
};

} // namespace alttech::crossvenue
