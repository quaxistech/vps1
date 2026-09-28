#pragma once
// Dynamic cross-venue lead-lag. No venue is assumed to be
// the permanent leader. Methods are bounded and CPU-safe: correlation is
// evaluated only over a small configured lag range on a bounded rolling window,
// never an open-ended cross-correlation.

#include "analytics_types.hpp"

#include <array>
#include <optional>
#include <span>

namespace alttech::crossvenue {

// One venue's aligned return series (same grid across venues) plus its measured
// clock offset, used to correct a false lead caused by clock skew.
struct VenueSeries {
    Venue venue = Venue::unknown;
    std::span<const double> returns;   // aligned, equal length across venues
    double clock_offset_ms = 0.0;      // exchange_time - local_time
};

struct LeadLagParams {
    int max_lag = 5;                   // bounded lag range in samples
    double sample_interval_ms = 1000;  // grid spacing (e.g. 1s)
    double min_abs_corr = 0.2;         // ignore weak/noise correlations
};

// Pearson correlation of a[t] and b[t-lag] over the overlapping region.
// nullopt when the overlap is < 2 or a series is constant.
[[nodiscard]] std::optional<double> correlation_at_lag(std::span<const double> a,
                                                       std::span<const double> b, int lag);

struct CrossLag {
    int lag = 0;            // >0: a follows b by `lag` samples (b leads a)
    double peak_corr = 0.0;
};

// Lag in [-max_lag, max_lag] maximizing |correlation| of a vs b.
[[nodiscard]] CrossLag best_cross_lag(std::span<const double> a, std::span<const double> b,
                                      int max_lag);

// Full four-venue lead-lag from aligned return series. Series with fewer than
// two points are ignored; the result masks them out.
[[nodiscard]] LeadLagResult compute_lead_lag(std::span<const VenueSeries> series,
                                             const LeadLagParams& params);

} // namespace alttech::crossvenue
