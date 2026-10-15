#pragma once

// The physical units a forecast and a plan are written in, and the one place
// they are converted.
//
// AltML publishes the same excursion twice: `expected_mfe` / `expected_mae`
// as a RETURN FRACTION (0.01 = 1 %) and `favorable_*_bps` /
// `expected_*_excursion_bps` in BASIS POINTS. The plan read the fraction as if
// it were bps and divided by 10 000 a second time, so a forecast 1 % move
// became a 0.0001 % target -- rounded onto the entry by the tick grid and then
// nudged one tick away: TARGET_FIRST at 0.1-1 bps against 15-20 bps of cost
// (QUALITY-01). Every conversion on that path now goes through these helpers,
// and every field that crosses it says its unit in its name.

#include <cmath>
#include <limits>

namespace altstrategy::engine::units {

inline constexpr double kBpsPerUnit = 10'000.0;

[[nodiscard]] constexpr double fraction_to_bps(double fraction) { return fraction * kBpsPerUnit; }
[[nodiscard]] constexpr double bps_to_fraction(double bps) { return bps / kBpsPerUnit; }

// A distance of `bps` basis points of `reference_price`, in price units:
// 100 bps of 100.0 is 1.0.
[[nodiscard]] constexpr double bps_to_price_distance(double reference_price, double bps) {
    return reference_price * bps / kBpsPerUnit;
}

// The size of a price distance in bps of `reference_price`; NaN when the
// reference is not a positive price, so a nonsense input cannot read as 0 bps.
[[nodiscard]] inline double price_distance_to_bps(double reference_price, double distance) {
    if (!(reference_price > 0)) return std::numeric_limits<double>::quiet_NaN();
    return std::abs(distance) / reference_price * kBpsPerUnit;
}

} // namespace altstrategy::engine::units
