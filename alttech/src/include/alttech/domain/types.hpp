#pragma once
// Wire-compatible market domain API.

#include "alttech/domain/decimal.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace alttech {

// ---------------------------------------------------------------------------
// Timeframes (value == minutes; matches proto enum values)
// ---------------------------------------------------------------------------
enum class Timeframe : int32_t {
    m1 = 1,
    m5 = 5,
    m15 = 15,
    m30 = 30,
    h1 = 60,
    h4 = 240,
    d1 = 1440,
};

inline constexpr std::array<Timeframe, 6> kDerivedTimeframes = {
    Timeframe::m5, Timeframe::m15, Timeframe::m30, Timeframe::h1, Timeframe::h4, Timeframe::d1};

[[nodiscard]] constexpr int32_t tf_minutes(Timeframe tf) noexcept { return static_cast<int32_t>(tf); }
[[nodiscard]] constexpr int64_t tf_ms(Timeframe tf) noexcept {
    return static_cast<int64_t>(tf) * 60'000;
}

[[nodiscard]] constexpr std::string_view tf_label(Timeframe tf) noexcept {
    switch (tf) {
        case Timeframe::m1: return "1m";
        case Timeframe::m5: return "5m";
        case Timeframe::m15: return "15m";
        case Timeframe::m30: return "30m";
        case Timeframe::h1: return "1h";
        case Timeframe::h4: return "4h";
        case Timeframe::d1: return "1d";
    }
    return "?";
}

[[nodiscard]] constexpr std::optional<Timeframe> tf_from_label(std::string_view s) noexcept {
    if (s == "1m") return Timeframe::m1;
    if (s == "5m") return Timeframe::m5;
    if (s == "15m") return Timeframe::m15;
    if (s == "30m") return Timeframe::m30;
    if (s == "1h") return Timeframe::h1;
    if (s == "4h") return Timeframe::h4;
    if (s == "1d") return Timeframe::d1;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Market data
// ---------------------------------------------------------------------------
enum class CandleSource : int32_t {
    unspecified = 0,
    ws = 1,
    rest_bootstrap = 2,
    rest_reconcile = 3,
    rest_repair = 4,
    aggregation = 5,
};

[[nodiscard]] constexpr std::string_view source_label(CandleSource s) noexcept {
    switch (s) {
        case CandleSource::ws: return "ws";
        case CandleSource::rest_bootstrap: return "rest_bootstrap";
        case CandleSource::rest_reconcile: return "rest_reconcile";
        case CandleSource::rest_repair: return "rest_repair";
        case CandleSource::aggregation: return "aggregation";
        case CandleSource::unspecified: return "unspecified";
    }
    return "?";
}

enum class Quality : int32_t { unspecified = 0, ok = 1, incomplete = 2, suspect = 3 };

[[nodiscard]] constexpr std::string_view quality_label(Quality q) noexcept {
    switch (q) {
        case Quality::ok: return "ok";
        case Quality::incomplete: return "incomplete";
        case Quality::suspect: return "suspect";
        case Quality::unspecified: return "unspecified";
    }
    return "?";
}

struct Candle {
    Timeframe timeframe = Timeframe::m1;
    int64_t open_time_ms = 0;   // inclusive UTC interval start
    int64_t close_time_ms = 0;  // exclusive UTC interval end
    Decimal open;
    Decimal high;
    Decimal low;
    Decimal close;
    Decimal base_volume;
    Decimal quote_volume;
    std::optional<int64_t> trade_count;
    std::optional<Decimal> taker_buy_base_volume;
    std::optional<Decimal> taker_sell_base_volume;
    bool is_closed = false;
    CandleSource source = CandleSource::unspecified;
    Quality quality = Quality::ok;
    int64_t revision = 0;
    std::optional<int64_t> minute_count; // aggregated bars: 1m candles in window

    // Structural validity: aligned times and OHLC consistency.
    [[nodiscard]] bool valid() const noexcept {
        const int64_t span = tf_ms(timeframe);
        if (span <= 0 || open_time_ms < 0) return false;
        if (open_time_ms % span != 0) return false;
        if (close_time_ms != open_time_ms + span) return false;
        if (high < open || high < close || high < low) return false;
        if (low > open || low > close) return false;
        if (base_volume.is_negative() || quote_volume.is_negative()) return false;
        if (open.is_negative() || high.is_negative() || low.is_negative() || close.is_negative())
            return false;
        return true;
    }
};

enum class TradeSide : int32_t { unspecified = 0, buy = 1, sell = 2 };

struct Trade {
    std::string exchange_trade_id;
    Decimal price;
    Decimal size;
    TradeSide side = TradeSide::unspecified;
    int64_t exchange_ts_ms = 0;
};

} // namespace alttech
