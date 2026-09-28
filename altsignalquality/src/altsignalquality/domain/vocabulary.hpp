#pragma once

// Stable, versioned encodings of the categorical vocabularies AltSignalQuality
// consumes from AltStrategy and AltTech.
//
// These ordinals are part of the feature contract: changing one silently
// re-labels every historical training row, so they are frozen alongside
// `kFeatureSchemaVersion` and covered by tests/unit/test_feature_contract.cpp.
// An unknown token maps to a MISSING sentinel and is never quietly folded into
// a neighbouring category.

#include <cstdint>
#include <string_view>

namespace altsignalquality::domain {

// Returned for any token outside the frozen vocabulary. Callers turn it into a
// missing feature (NaN + cleared validity bit), never into a number.
inline constexpr double kUnknownOrdinal = -1.0;

// Proposal strength, ordered by conviction. INVALID never reaches this service
// (AltStrategy refuses to publish it) but is encoded for completeness.
[[nodiscard]] constexpr double grade_ordinal(std::string_view grade) noexcept {
    if (grade == "SHADOW") return 0.0;
    if (grade == "EXPERIMENTAL") return 1.0;
    if (grade == "WEAK") return 2.0;
    if (grade == "NORMAL") return 3.0;
    if (grade == "STRONG") return 4.0;
    if (grade == "INVALID") return -2.0;
    return kUnknownOrdinal;
}

// altstrategy.v1.EntryType.
[[nodiscard]] constexpr double entry_type_ordinal(int entry_type) noexcept {
    switch (entry_type) {
        case 1: return 0.0;  // ENTRY_MARKET
        case 2: return 1.0;  // ENTRY_LIMIT
        case 3: return 2.0;  // ENTRY_STOP_MARKET
        case 4: return 3.0;  // ENTRY_STOP_LIMIT
        default: return kUnknownOrdinal;
    }
}

// altstrategy.v1.HoldingClass — ordered by horizon length.
[[nodiscard]] constexpr double holding_class_ordinal(int holding_class) noexcept {
    switch (holding_class) {
        case 1: return 0.0;  // HOLDING_SCALP
        case 2: return 1.0;  // HOLDING_INTRADAY
        case 3: return 2.0;  // HOLDING_EXTENDED_INTRADAY
        default: return kUnknownOrdinal;
    }
}

// Cross-venue aggregate quality, ordered by how many venues agreed. Higher is
// better, so the ordinal is monotone in information quality.
[[nodiscard]] constexpr double context_quality_ordinal(std::string_view quality) noexcept {
    if (quality == "INVALID") return 0.0;
    if (quality == "DEGRADED_2_OF_4") return 1.0;
    if (quality == "DEGRADED_3_OF_4") return 2.0;
    if (quality == "STRICT_4_OF_4") return 3.0;
    return kUnknownOrdinal;
}

// AltTech's GlobalRegime / MarketRegime share one vocabulary and one numbering
// (alttech/proto/regime.proto); the ordinals below ARE those enum values, so a
// regime string and the producer's enum can never drift apart.
[[nodiscard]] constexpr double regime_ordinal(std::string_view regime) noexcept {
    if (regime == "UNDEFINED") return 1.0;
    if (regime == "LOW_LIQUIDITY") return 2.0;
    if (regime == "CHAOTIC") return 3.0;
    if (regime == "TREND_UP") return 4.0;
    if (regime == "TREND_DOWN") return 5.0;
    if (regime == "BREAKOUT") return 6.0;
    if (regime == "VOLATILITY_EXPANSION") return 7.0;
    if (regime == "VOLATILITY_COMPRESSION") return 8.0;
    if (regime == "RANGE") return 9.0;
    return kUnknownOrdinal;
}

[[nodiscard]] constexpr bool is_known_regime(std::string_view regime) noexcept {
    return regime_ordinal(regime) != kUnknownOrdinal;
}

// altstrategy.v1.Side. LONG is the positive direction everywhere in this
// service, including the signed price-offset features.
[[nodiscard]] constexpr double side_sign(int side) noexcept {
    return side == 2 ? -1.0 : 1.0;  // SIDE_SHORT = 2
}

[[nodiscard]] constexpr std::string_view side_name(int side) noexcept {
    return side == 2 ? "short" : "long";
}

// Population count of AltStrategy's venue bitmask (4 venues).
[[nodiscard]] constexpr double venue_count(uint32_t mask) noexcept {
    double n = 0;
    for (uint32_t m = mask; m != 0; m &= m - 1) n += 1.0;
    return n;
}

} // namespace altsignalquality::domain
