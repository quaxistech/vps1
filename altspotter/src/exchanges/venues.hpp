#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace altspotter::exchanges {

// The four supported USDT-perpetual futures venues. Numeric ids match both the
// PostgreSQL `exchanges.id` seed (migration 0001) and the proto Exchange enum.
// `code` is the DB code and the NATS subject label (md.<code>.<symbol>.*).
// Spot markets are intentionally out of scope.
struct VenueInfo {
    int16_t exchange_id;
    std::string_view code;          // DB code + subject label
    std::string_view display_name;
    std::string_view market_type;   // always usdt_perp
};

inline constexpr int16_t kVenueBitget = 1;
inline constexpr int16_t kVenueBinanceUsdm = 2;
inline constexpr int16_t kVenueOkx = 3;
inline constexpr int16_t kVenueBybit = 4;

inline constexpr std::array<VenueInfo, 4> kVenues = {{
    {kVenueBitget, "bitget", "Bitget USDT-M Futures", "usdt_perp"},
    {kVenueBinanceUsdm, "binance_usdm", "Binance USDS-M Futures", "usdt_perp"},
    {kVenueOkx, "okx", "OKX USDT Perpetual Swap", "usdt_perp"},
    {kVenueBybit, "bybit", "Bybit Linear USDT Perpetual", "usdt_perp"},
}};

[[nodiscard]] constexpr const VenueInfo* venue_by_id(int16_t exchange_id) noexcept {
    for (const auto& v : kVenues) {
        if (v.exchange_id == exchange_id) return &v;
    }
    return nullptr;
}

[[nodiscard]] constexpr const VenueInfo* venue_by_code(std::string_view code) noexcept {
    for (const auto& v : kVenues) {
        if (v.code == code) return &v;
    }
    return nullptr;
}

[[nodiscard]] constexpr std::string_view venue_code(int16_t exchange_id) noexcept {
    const auto* v = venue_by_id(exchange_id);
    return v != nullptr ? v->code : std::string_view{"?"};
}

} // namespace altspotter::exchanges
