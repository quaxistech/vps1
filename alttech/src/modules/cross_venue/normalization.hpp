#pragma once
// Cross-venue normalization. Different venues use
// different funding intervals, contract semantics and native units; nothing is
// summed in native units and funding is never mixed across settlement meanings.

#include <optional>
#include <span>

namespace alttech::crossvenue {

// --- funding --------------------------------------------------
struct NormalizedFunding {
    double rate_per_hour = 0.0;
    double rate_8h_equivalent = 0.0;
    double annualized = 0.0;        // additional analytical field only
};

// Normalize a venue funding rate to comparable units. `interval_hours` must be
// > 0 (e.g. 1, 4, 8); an unknown/zero interval yields nullopt (never guessed).
[[nodiscard]] std::optional<NormalizedFunding> normalize_funding(double rate,
                                                                 double interval_hours);

// --- open interest --------------------------------------------
// Relative change (oi - previous)/previous; nullopt when previous is not > 0.
[[nodiscard]] std::optional<double> oi_relative_change(double oi_usd, double previous_oi_usd);

// --- generic robust normalization ------------------------------------------
// z-score of x against a rolling window; nullopt when < 2 samples or zero std.
[[nodiscard]] std::optional<double> zscore(double x, std::span<const double> window);

// Percentile rank (0..1) of x within window (fraction of samples <= x).
[[nodiscard]] double percentile_rank(double x, std::span<const double> window);

// Population mean / standard deviation of a window (std=0 for < 2 samples).
[[nodiscard]] double mean(std::span<const double> window);
[[nodiscard]] double stddev(std::span<const double> window);

} // namespace alttech::crossvenue
