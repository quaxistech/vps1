#pragma once
// Per-venue reference feature and regime engine.
//
// Pure: it takes a bounded candle window plus the target bar's aggregate
// context and produces the ordered reference vector, a deterministic venue
// regime and a swing zone. No PostgreSQL or NATS dependency, so the analytics
// are unit-testable in isolation.

#include "reference_features.hpp"

#include "alttech/domain/venue.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace alttech::venuelite {

// One 1m candle. Analytics precision is double; exact decimals stay in the
// NUMERIC market-data columns and are never round-tripped through float here.
struct LiteCandle {
    int64_t open_time_ms = 0;
    double open = 0, high = 0, low = 0, close = 0;
    double base_volume = 0, quote_volume = 0;
    double taker_buy_base_volume = 0, taker_sell_base_volume = 0;
    int64_t revision = 0;
    bool quality_ok = true;
};

// AltSpotter-derived aggregate context for the target bar; any field may be
// absent, and an absent field leaves its feature invalid rather than zero.
struct LiteContext {
    // trade aggregates
    std::optional<double> buy_volume;
    std::optional<double> sell_volume;
    std::optional<double> signed_volume_base;
    std::optional<double> vwap;
    std::optional<double> trade_count;
    // book aggregates
    std::optional<double> spread;
    std::optional<double> relative_spread;
    std::optional<double> bid_depth5;
    std::optional<double> ask_depth5;
    std::optional<double> book_imbalance;
    std::optional<double> microprice;
    std::optional<double> mid_price;
    // derivatives context
    std::optional<double> mark_price;
    std::optional<double> index_price;

    // Intra-minute top-of-book, from market_bbo_1m. Absent when the venue
    // published no BBO for the minute -- left unset, never defaulted, because
    // a zero spread is a claim about the market rather than a missing reading.
    std::optional<double> bbo_bid_first;
    std::optional<double> bbo_ask_first;
    std::optional<double> bbo_bid_last;
    std::optional<double> bbo_ask_last;
    std::optional<double> bbo_spread_min;
    std::optional<double> bbo_spread_max;
    std::optional<double> bbo_sample_count;

    // Second-level summary of the minute, from the 1s aggregate series.
    std::optional<double> flow_peak_trades_1s;
    std::optional<double> flow_peak_volume_1s;
    std::optional<double> flow_mean_volume_1s;
    std::optional<double> flow_active_seconds;
    std::optional<double> flow_signed_stddev;
    std::optional<double> book_depth_min_1s;
    std::optional<double> book_depth_mean_1s;
    std::optional<double> funding_rate;
    std::optional<double> funding_interval_hours;
    std::optional<double> oi_change_ratio;
    // quality
    double data_quality = 1.0; // 1 valid, 0.5 degraded, 0 otherwise
};

enum class VenueRegime : int32_t {
    undefined = 0,
    trend_up,
    trend_down,
    range,
    breakout,
    volatility_expansion,
    volatility_compression,
    chaotic,
    low_liquidity,
};

[[nodiscard]] std::string_view regime_label(VenueRegime r);

struct RefResult {
    std::vector<float> values;      // RF_FEATURE_COUNT wide
    std::vector<uint8_t> validity;  // bitmap: bit i set means values[i] is usable
    bool warming = false;           // insufficient history for the full set
    VenueRegime regime = VenueRegime::undefined;
    double regime_confidence = 0.0;
    double trend_strength = 0.0;    // EMA(20)-EMA(200) spread, in ATR units
    double volatility_regime = 0.0; // empirical percentile of Bollinger bandwidth
    double liquidity_quality = 0.0;
};

// Compute the reference vector, venue regime and axes. `window` is
// oldest-to-newest and must end at the target bar. Indicators without enough
// history are left invalid rather than approximated.
[[nodiscard]] RefResult compute_reference(const std::vector<LiteCandle>& window,
                                          const LiteContext& ctx);

// BBO dynamics only, for a venue whose indicator ladder is already produced by
// the full TA-Lib engine.
//
// market_bbo_1m is the one input the full engine cannot see: it works from
// candles, and BBO is a summary of the continuous quote stream. Without this
// entry point the PRIMARY venue — the one every order is actually sent to —
// would be the only venue with no intra-minute spread analytics at all, while
// the three information venues had them.
//
// Only the BBO block is populated; every other position stays invalid, so no
// indicator is computed twice in two modules.
[[nodiscard]] RefResult compute_bbo_only(const LiteContext& ctx);

// A compact swing support/resistance zone in venue-local prices.
struct SwingZone {
    bool resistance = true;
    double lower = 0, center = 0, upper = 0;
    double strength = 0; // [0,1]: fraction of recent bars that touched the band
    // Evidence, DERIVED from the window every time rather than accumulated
    // across cycles — the same rule the structure module states for itself in
    // zone_lifecycle.hpp. A counter incremented once per sweep measures how
    // often the sweep ran, not how often price respected the band.
    int touch_count = 0;     // bars whose range intersected the band
    int reaction_count = 0;  // touches price then rejected by >= half an ATR
};

// Detect the nearer recent swing extreme as a banded venue zone. nullopt when
// the window is too short.
[[nodiscard]] std::optional<SwingZone> detect_swing_zone(const std::vector<LiteCandle>& window);

} // namespace alttech::venuelite
