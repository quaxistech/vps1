#pragma once
// Venue identity shared by every four-venue analytics path.
//
// Numeric values match PostgreSQL exchanges.id and the alttech.v1 Exchange enum
// (1 Bitget, 2 Binance USDⓈ-M, 3 OKX, 4 Bybit). The venue bit used in every
// cross-venue mask is (id - 1): bit 0 Bitget, bit 1 Binance, bit 2 OKX,
// bit 3 Bybit. Bitget is the single primary trading venue; the other three are
// reference venues.

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace alttech {

enum class Venue : int32_t {
    unknown = 0,
    bitget = 1,
    binance_usdm = 2,
    okx = 3,
    bybit = 4,
};

inline constexpr Venue kPrimaryVenue = Venue::bitget;

inline constexpr std::array<Venue, 4> kAllVenues = {Venue::bitget, Venue::binance_usdm,
                                                    Venue::okx, Venue::bybit};

inline constexpr std::array<Venue, 3> kReferenceVenues = {Venue::binance_usdm, Venue::okx,
                                                          Venue::bybit};

[[nodiscard]] constexpr int32_t venue_id(Venue v) noexcept { return static_cast<int32_t>(v); }

// The regime, structure and microstructure modules are primary-venue-only by
// design, and their SQL says so with a literal `exchange_id = 1` (19 sites).
// Those literals cannot follow a constant without turning compile-time query
// strings into runtime concatenation in hot paths, so the coupling is asserted
// here instead: if the primary venue ever stops being id 1, this breaks the
// build at the one place that explains why, rather than letting those queries
// silently read a venue nobody meant.
static_assert(venue_id(kPrimaryVenue) == 1,
              "primary-venue-only SQL hardcodes exchange_id = 1; update those "
              "queries (regime/structure/microstructure) before changing this");

// Bit position of the venue in a cross-venue mask (id - 1). unknown -> -1.
[[nodiscard]] constexpr int venue_bit(Venue v) noexcept {
    return v == Venue::unknown ? -1 : venue_id(v) - 1;
}

// Single-venue mask (0 for unknown).
[[nodiscard]] constexpr uint32_t venue_mask_bit(Venue v) noexcept {
    return v == Venue::unknown ? 0U : (1U << static_cast<unsigned>(venue_bit(v)));
}

[[nodiscard]] constexpr bool is_primary(Venue v) noexcept { return v == kPrimaryVenue; }
[[nodiscard]] constexpr bool is_reference(Venue v) noexcept {
    return v == Venue::binance_usdm || v == Venue::okx || v == Venue::bybit;
}

// AltSpotter venue codes (the strings used in NATS subjects and exchanges.code).
[[nodiscard]] constexpr std::string_view venue_code(Venue v) noexcept {
    switch (v) {
        case Venue::bitget: return "bitget";
        case Venue::binance_usdm: return "binance_usdm";
        case Venue::okx: return "okx";
        case Venue::bybit: return "bybit";
        case Venue::unknown: return "unknown";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<Venue> venue_from_code(std::string_view code) noexcept {
    if (code == "bitget") return Venue::bitget;
    if (code == "binance_usdm" || code == "binance") return Venue::binance_usdm;
    if (code == "okx") return Venue::okx;
    if (code == "bybit") return Venue::bybit;
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<Venue> venue_from_id(int32_t id) noexcept {
    switch (id) {
        case 1: return Venue::bitget;
        case 2: return Venue::binance_usdm;
        case 3: return Venue::okx;
        case 4: return Venue::bybit;
        default: return std::nullopt;
    }
}

// Partition (and therefore retention) class of a venue's feature vectors.
//
// This split is about STORAGE, not about what is computed: every venue
// gets the full TA-Lib engine. Primary and reference vectors live in
// different partitions so retention can treat them differently.
[[nodiscard]] constexpr std::string_view venue_feature_class(Venue v) noexcept {
    return is_primary(v) ? "bitget" : "reference";
}

// Mask over all four venues (0b1111).
inline constexpr uint32_t kAllVenuesMask = 0b1111U;

[[nodiscard]] constexpr int mask_popcount(uint32_t mask) noexcept {
    int count = 0;
    for (uint32_t bits = mask & kAllVenuesMask; bits; bits &= bits - 1) ++count;
    return count;
}

} // namespace alttech
