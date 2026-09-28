#pragma once

#include "core/decimal.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace altspotter {

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

// ---------------------------------------------------------------------------
// Instruments
// ---------------------------------------------------------------------------
enum class InstrumentStatus : int32_t {
    unspecified = 0,
    active = 1,
    suspended = 2,
    delisted = 3,
    not_listed = 4,
};

[[nodiscard]] constexpr std::string_view instrument_status_label(InstrumentStatus s) noexcept {
    switch (s) {
        case InstrumentStatus::active: return "active";
        case InstrumentStatus::suspended: return "suspended";
        case InstrumentStatus::delisted: return "delisted";
        case InstrumentStatus::not_listed: return "not_listed";
        case InstrumentStatus::unspecified: return "unspecified";
    }
    return "?";
}

struct InstrumentSpec {
    std::string exchange_symbol;
    std::string base_asset;
    std::string quote_asset;
    std::string settle_asset = "USDT";
    std::string contract_type = "perpetual"; // linear USDT perpetual on all venues
    Decimal contract_value{1, 0}; // base units per contract (OKX ctVal; 1 elsewhere)
    InstrumentStatus status = InstrumentStatus::unspecified;
    int64_t list_time_ms = 0;
    Decimal tick_size;
    int32_t price_precision = 0;
    Decimal quantity_step;
    int32_t quantity_precision = 0;
    Decimal min_order_quantity;
    Decimal min_notional;
    Decimal maker_fee_rate;
    Decimal taker_fee_rate;
    int32_t min_leverage = 0;
    int32_t max_leverage = 0;
    int32_t funding_interval_hours = 0;

    // Canonical string for change detection (hashed with SHA-256).
    [[nodiscard]] std::string normalized_fingerprint() const;
};

struct InstrumentRef {
    int64_t instrument_id = 0;
    int16_t exchange_id = 0;
    std::string exchange_symbol;
    std::string base_asset;
    InstrumentStatus status = InstrumentStatus::unspecified;
    int64_t list_time_ms = 0;
};

} // namespace altspotter
