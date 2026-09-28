#pragma once
// Quality-aware cross-venue consensus.
//
// Deterministic and reproducible: the same inputs always produce the same
// weights, the same estimator output and the same recorded per-venue
// contributions. No arithmetic mean is used; the production default is a
// quality/liquidity-weighted median.

#include "analytics_types.hpp"

#include <functional>
#include <span>
#include <vector>

namespace alttech::crossvenue {

// Tunable weight scales. Every scale is the value at which its factor halves,
// so the factors are comparable and none can dominate the others.
struct WeightParams {
    // A venue whose data is one bar interval behind counts at half weight.
    double freshness_scale_ms = 60000.0;
    double spread_scale = 0.0005;         // 5 bps relative spread halves weight
    double depth_ref_usdt = 50000.0;      // depth giving 0.5 liquidity factor
    double clock_offset_scale_ms = 250.0; // |offset| halving weight
    double degraded_quality_factor = 0.5; // DEGRADED venues count at half weight
    double incomplete_payload_factor = 0.5;
};

// Bounds used by quality classification, supplied by the caller from config.
struct QualityParams {
    // Maximum spread between the participating venues' clock offsets. Above
    // this the venues are not describing the same instant and the window
    // cannot be STRICT_VALID.
    double max_cross_venue_skew_ms = 2500.0;
    // How far behind its own market data a venue may be and still count. It is
    // measured against the 1m grid the cross-venue analytics run on, so the
    // default is three bar intervals.
    double max_venue_data_age_ms = 180000.0;
};

// Non-negative quality/liquidity weight for one venue.
// Returns 0 for an unusable venue so it cannot enter the estimator.
[[nodiscard]] double venue_weight(const VenueSample& sample, const WeightParams& params);

// The three supported estimators over (value, weight) pairs. Zero-weight or
// empty input yields {available=false}. `trim_fraction` in (0,0.5) for the
// trimmed mean removes that value-tail fraction of total weight from each end.
[[nodiscard]] ConsensusResult weighted_median(std::span<const Contribution> included);
[[nodiscard]] ConsensusResult weighted_mean(std::span<const Contribution> included);
[[nodiscard]] ConsensusResult trimmed_weighted_mean(std::span<const Contribution> included,
                                                    double trim_fraction = 0.25);

// Build the consensus of one metric across venues. `pick` extracts the metric
// (nullopt = venue does not supply it, excluded with reason). Only usable
// venues with a positive weight and a present metric contribute. The chosen
// method drives the estimator; contributions are always recorded.
[[nodiscard]] ConsensusResult consensus(
    std::span<const VenueSample> samples, const WeightParams& weights, ConsensusMethod method,
    const std::function<std::optional<double>(const VenueSample&)>& pick);

// Aggregate quality decision + venue masks for a canonical window.
// `max_skew_ms` is the observed cross-venue event-time skew for the window.
[[nodiscard]] QualityDecision classify_quality(std::span<const VenueSample> samples,
                                               const QualityParams& params, double max_skew_ms);

// Deviation of `value` from `consensus` in basis points. consensus must be > 0.
[[nodiscard]] double deviation_bps(double value, double consensus);

// Weighted dispersion (population standard deviation) of the included values
// around the consensus, expressed in bps of the consensus. 0 when < 2 venues.
[[nodiscard]] double dispersion_bps(std::span<const Contribution> included, double consensus);

} // namespace alttech::crossvenue
