#include "consensus.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace alttech::crossvenue {
namespace {

// Bounded [0,1] decay factor: 1 at x=0, 0.5 at x=scale, -> 0 as x grows.
[[nodiscard]] double half_life_factor(double x, double scale) {
    if (scale <= 0.0) return 1.0;
    const double v = std::abs(x);
    return scale / (scale + v);
}

[[nodiscard]] double quality_factor(VenueQuality q, const WeightParams& p) {
    switch (q) {
        case VenueQuality::valid: return 1.0;
        case VenueQuality::degraded: return p.degraded_quality_factor;
        default: return 0.0;
    }
}

} // namespace

double venue_weight(const VenueSample& sample, const WeightParams& params) {
    if (!sample.present || !usable(sample.quality)) return 0.0;
    double weight = std::clamp(sample.historical_reliability, 0.0, 1.0);
    weight *= quality_factor(sample.quality, params);
    if (weight <= 0.0) return 0.0;

    weight *= half_life_factor(sample.data_age_ms, params.freshness_scale_ms);
    weight *= half_life_factor(sample.clock_offset_ms, params.clock_offset_scale_ms);

    // Liquidity: tighter spread and deeper book raise the weight. Absent metrics
    // are treated neutrally (factor 1) rather than penalized to zero.
    //
    // "Absent" includes a reading of zero. A zero depth is not the statement
    // "this venue has an empty book", it is the absence of a measurement — and
    // depth/(depth + ref) evaluates to exactly 0 for it, which zeroes the whole
    // weight and silently drops the venue from the consensus. When the upstream
    // book aggregate reported no depth at all, that vetoed every venue on every
    // pair: `zero_weight` on all 3024 contributions, no consensus price, no
    // consensus funding, and a mark_price of 0 in every downstream decision
    // context. One unavailable liquidity metric must never be able to do that.
    if (sample.relative_spread && *sample.relative_spread > 0.0) {
        weight *= half_life_factor(*sample.relative_spread, params.spread_scale);
    }
    if (sample.depth_usdt && *sample.depth_usdt > 0.0) {
        const double depth = *sample.depth_usdt;
        weight *= depth / (depth + params.depth_ref_usdt);
    }
    if (!sample.payload_complete) weight *= params.incomplete_payload_factor;
    return std::max(0.0, weight);
}

ConsensusResult weighted_median(std::span<const Contribution> included) {
    ConsensusResult result;
    if (included.empty()) return result;
    std::vector<Contribution> sorted(included.begin(), included.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const Contribution& a, const Contribution& b) { return a.value < b.value; });
    double total = 0.0;
    for (const auto& c : sorted) total += c.weight;
    if (total <= 0.0) return result;

    const double half = total / 2.0;
    double cumulative = 0.0;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        cumulative += sorted[i].weight;
        if (cumulative > half) {
            result.available = true;
            result.value = sorted[i].value;
            break;
        }
        if (cumulative == half) {
            // Exact split: average this value and the next (lower weighted median
            // convention with interpolation) for determinism.
            const double next = (i + 1 < sorted.size()) ? sorted[i + 1].value : sorted[i].value;
            result.available = true;
            result.value = 0.5 * (sorted[i].value + next);
            break;
        }
    }
    result.contributions.assign(included.begin(), included.end());
    return result;
}

ConsensusResult weighted_mean(std::span<const Contribution> included) {
    ConsensusResult result;
    double total = 0.0;
    double acc = 0.0;
    for (const auto& c : included) {
        total += c.weight;
        acc += c.weight * c.value;
    }
    if (total <= 0.0) return result;
    result.available = true;
    result.value = acc / total;
    result.contributions.assign(included.begin(), included.end());
    return result;
}

ConsensusResult trimmed_weighted_mean(std::span<const Contribution> included,
                                      double trim_fraction) {
    ConsensusResult result;
    if (included.empty()) return result;
    trim_fraction = std::clamp(trim_fraction, 0.0, 0.49);
    std::vector<Contribution> sorted(included.begin(), included.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const Contribution& a, const Contribution& b) { return a.value < b.value; });
    double total = 0.0;
    for (const auto& c : sorted) total += c.weight;
    if (total <= 0.0) return result;

    const double cut = total * trim_fraction;
    double lo = 0.0;
    double hi = 0.0;
    double acc = 0.0;
    double kept = 0.0;
    // Trim `cut` weight from each value tail, keep the middle mass.
    for (const auto& c : sorted) {
        const double start = lo;
        const double end = lo + c.weight;
        lo = end;
        // Weight of this entry that survives both tail cuts.
        const double keep_lo = std::max(start, cut);
        const double keep_hi = std::min(end, total - cut);
        const double w = std::max(0.0, keep_hi - keep_lo);
        if (w > 0.0) {
            acc += w * c.value;
            kept += w;
        }
        hi = end;
    }
    (void)hi;
    if (kept <= 0.0) return weighted_median(included); // degenerate: fall back
    result.available = true;
    result.value = acc / kept;
    result.contributions.assign(included.begin(), included.end());
    return result;
}

ConsensusResult consensus(std::span<const VenueSample> samples, const WeightParams& weights,
                          ConsensusMethod method,
                          const std::function<std::optional<double>(const VenueSample&)>& pick) {
    std::vector<Contribution> all;
    std::vector<Contribution> included;
    all.reserve(samples.size());
    included.reserve(samples.size());
    for (const auto& s : samples) {
        Contribution c;
        c.venue = s.venue;
        c.quality = s.quality;
        c.freshness_ms = s.data_age_ms;
        c.relative_spread = s.relative_spread.value_or(0.0);
        c.depth_usdt = s.depth_usdt.value_or(0.0);
        const auto metric = pick(s);
        const double w = venue_weight(s, weights);
        if (!s.present) {
            c.exclude_reason = "absent";
        } else if (!usable(s.quality)) {
            c.exclude_reason = quality_label(s.quality);
        } else if (!metric) {
            c.exclude_reason = "metric_unavailable";
        } else if (w <= 0.0) {
            c.exclude_reason = "zero_weight";
        } else {
            c.value = *metric;
            c.weight = w;
            c.included = true;
        }
        all.push_back(c);
        if (c.included) included.push_back(c);
    }

    ConsensusResult result;
    switch (method) {
        case ConsensusMethod::quality_weighted_mean:
            result = weighted_mean(included);
            break;
        case ConsensusMethod::trimmed_weighted_mean:
            result = trimmed_weighted_mean(included);
            break;
        case ConsensusMethod::quality_weighted_median:
        default:
            result = weighted_median(included);
            break;
    }
    // Always report every venue's contribution (reproducibility).
    result.contributions = std::move(all);
    if (result.available) {
        double total = 0.0;
        for (const auto& c : included) total += c.weight;
        // Confidence rises with participating mass and venue count.
        const double coverage = static_cast<double>(included.size()) /
                                static_cast<double>(kAllVenues.size());
        result.confidence = std::clamp(coverage * (total > 0.0 ? 1.0 : 0.0), 0.0, 1.0);
    }
    return result;
}

QualityDecision classify_quality(std::span<const VenueSample> samples, const QualityParams& params,
                                 double max_skew_ms) {
    QualityDecision d;
    bool bitget_valid = false;
    int usable_references = 0;
    for (const auto& s : samples) {
        const uint32_t bit = venue_mask_bit(s.venue);
        if (!s.present) {
            d.missing_venue_mask |= bit;
            d.reasons.emplace_back(std::string(venue_code(s.venue)) + ":missing");
            continue;
        }
        if (s.quality == VenueQuality::stale) d.stale_venue_mask |= bit;
        if (s.quality == VenueQuality::desynced) d.desynced_venue_mask |= bit;
        if (usable(s.quality) && s.data_age_ms <= params.max_venue_data_age_ms) {
            d.venue_mask |= bit;
            if (is_primary(s.venue)) bitget_valid = true;
            if (is_reference(s.venue)) ++usable_references;
        } else if (!usable(s.quality)) {
            d.reasons.emplace_back(std::string(venue_code(s.venue)) + ":" +
                                   std::string(quality_label(s.quality)));
        } else {
            d.reasons.emplace_back(std::string(venue_code(s.venue)) + ":data_too_old");
        }
    }

    const bool skew_ok = max_skew_ms <= params.max_cross_venue_skew_ms;
    if (!skew_ok) d.reasons.emplace_back("skew_exceeded");

    if (!bitget_valid) {
        d.status = CrossVenueQuality::invalid;
        d.reasons.emplace_back("bitget_invalid");
    } else if (usable_references >= 3 && skew_ok) {
        d.status = CrossVenueQuality::strict_valid;
    } else if (usable_references >= 2) {
        d.status = CrossVenueQuality::degraded_3_of_4;
    } else if (usable_references >= 1) {
        d.status = CrossVenueQuality::degraded_2_of_4;
    } else {
        d.status = CrossVenueQuality::invalid;
        d.reasons.emplace_back("no_valid_reference");
    }

    const int usable_count = mask_popcount(d.venue_mask);
    d.confidence = std::clamp(static_cast<double>(usable_count) / 4.0 * (skew_ok ? 1.0 : 0.6),
                              0.0, 1.0);
    return d;
}

double deviation_bps(double value, double consensus) {
    if (consensus == 0.0) return 0.0;
    return (value - consensus) / consensus * 1.0e4;
}

double dispersion_bps(std::span<const Contribution> included, double consensus) {
    if (included.size() < 2 || consensus == 0.0) return 0.0;
    double total = 0.0;
    double acc = 0.0;
    for (const auto& c : included) {
        total += c.weight;
        const double d = deviation_bps(c.value, consensus);
        acc += c.weight * d * d;
    }
    if (total <= 0.0) return 0.0;
    return std::sqrt(acc / total);
}

} // namespace alttech::crossvenue
