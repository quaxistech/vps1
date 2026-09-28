#include "global_view.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace alttech::crossvenue {
namespace {

// A venue with no usable weight must not silently disappear from the vote, but
// it must not dominate either: an absent weight is treated as the smallest
// meaningful contribution rather than as zero, so a fully degraded window still
// produces a defined (low-confidence) answer instead of no answer at all.
constexpr double kMinVoteWeight = 1e-6;

double weight_of(double weight) { return std::max(weight, kMinVoteWeight); }

} // namespace

GlobalRegimeResult aggregate_regimes(std::span<const VenueRegimeInput> inputs) {
    GlobalRegimeResult result;
    if (inputs.empty()) return result;

    std::map<std::string, double> votes;
    std::map<std::string, uint32_t> voters;
    double total = 0.0;
    double trend = 0.0;
    double volatility = 0.0;
    double liquidity = 0.0;
    double confidence = 0.0;
    std::string bitget_regime;

    for (const auto& in : inputs) {
        if (in.venue == Venue::unknown || in.regime.empty() || in.regime == "UNDEFINED") continue;
        const double w = weight_of(in.weight);
        votes[in.regime] += w;
        voters[in.regime] |= venue_mask_bit(in.venue);
        total += w;
        trend += w * in.trend_strength;
        volatility += w * in.volatility_regime;
        liquidity += w * in.liquidity_quality;
        confidence += w * in.confidence;
        if (is_primary(in.venue)) bitget_regime = in.regime;
    }
    if (total <= 0.0 || votes.empty()) return result;

    // Winner is the regime carrying the most weight; ties break on the regime
    // name so the result is deterministic and reproducible.
    auto best = votes.begin();
    for (auto it = votes.begin(); it != votes.end(); ++it) {
        if (it->second > best->second) best = it;
    }

    result.available = true;
    result.primary_regime = best->first;
    result.venue_agreement = best->second / total;
    result.supporting_venue_mask = voters[best->first];
    result.trend_consensus = trend / total;
    result.volatility_consensus = volatility / total;
    result.liquidity_consensus = liquidity / total;
    result.regime_confidence = result.venue_agreement * (confidence / total);
    result.bitget_divergence = !bitget_regime.empty() && bitget_regime != result.primary_regime;

    // Normalized Shannon entropy over the weighted regime distribution.
    if (votes.size() > 1) {
        double entropy = 0.0;
        for (const auto& [_, w] : votes) {
            const double p = w / total;
            if (p > 0.0) entropy -= p * std::log(p);
        }
        result.venue_dispersion =
            std::clamp(entropy / std::log(static_cast<double>(votes.size())), 0.0, 1.0);
    }
    return result;
}

std::vector<ConsensusZone> build_consensus_zones(std::span<const VenueZoneInput> zones,
                                                 int min_venues) {
    struct Normalized {
        Venue venue = Venue::unknown;
        std::string kind;
        double center = 0.0;
        double lower = 0.0;
        double upper = 0.0;
        double strength = 0.0;
        double weight = 0.0;
    };

    std::vector<Normalized> normalized;
    normalized.reserve(zones.size());
    double total_weight = 0.0;
    for (const auto& z : zones) {
        if (z.venue == Venue::unknown || !(z.venue_mid > 0.0) || z.kind.empty()) continue;
        if (!(z.lower <= z.center && z.center <= z.upper)) continue;
        Normalized n;
        n.venue = z.venue;
        n.kind = z.kind;
        // Removing the venue's own mid price removes its basis: what remains is
        // the zone's position relative to that venue's market.
        n.center = z.center / z.venue_mid - 1.0;
        n.lower = z.lower / z.venue_mid - 1.0;
        n.upper = z.upper / z.venue_mid - 1.0;
        n.strength = z.strength;
        n.weight = weight_of(z.weight);
        total_weight += n.weight;
        normalized.push_back(std::move(n));
    }
    if (normalized.empty() || total_weight <= 0.0) return {};

    std::sort(normalized.begin(), normalized.end(), [](const Normalized& a, const Normalized& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.center < b.center;
    });

    std::vector<ConsensusZone> out;
    std::size_t start = 0;
    while (start < normalized.size()) {
        std::size_t end = start + 1;
        double cluster_upper = normalized[start].upper;
        while (end < normalized.size() && normalized[end].kind == normalized[start].kind &&
               normalized[end].lower <= cluster_upper) {
            cluster_upper = std::max(cluster_upper, normalized[end].upper);
            ++end;
        }

        uint32_t mask = 0;
        double weight = 0.0;
        double center = 0.0;
        double width = 0.0;
        double strength = 0.0;
        for (std::size_t i = start; i < end; ++i) {
            mask |= venue_mask_bit(normalized[i].venue);
            weight += normalized[i].weight;
            center += normalized[i].weight * normalized[i].center;
            width += normalized[i].weight * (normalized[i].upper - normalized[i].lower);
            strength += normalized[i].weight * normalized[i].strength;
        }
        const int venues = mask_popcount(mask);
        if (venues >= min_venues && weight > 0.0) {
            ConsensusZone z;
            z.kind = normalized[start].kind;
            z.normalized_center = center / weight;
            z.normalized_width = width / weight;
            z.supporting_venue_mask = mask;
            z.supporting_venue_count = venues;
            z.venue_confluence_score = std::clamp(weight / total_weight, 0.0, 1.0);
            z.strength = std::clamp(strength / weight, 0.0, 1.0);
            // Confidence combines how many venues agree with how strong the
            // agreed level is on each of them.
            z.confidence = std::clamp(
                (static_cast<double>(venues) / static_cast<double>(kAllVenues.size())) * z.strength,
                0.0, 1.0);
            out.push_back(std::move(z));
        }
        start = end;
    }
    return out;
}

ProjectedZone project_to_primary(const ConsensusZone& zone, double primary_mid,
                                 double consensus_mid, double window_confidence) {
    ProjectedZone p;
    if (!(primary_mid > 0.0)) return p;
    const double half = std::abs(zone.normalized_width) / 2.0;
    p.center = primary_mid * (1.0 + zone.normalized_center);
    p.lower = primary_mid * (1.0 + zone.normalized_center - half);
    p.upper = primary_mid * (1.0 + zone.normalized_center + half);
    p.basis_adjustment = consensus_mid > 0.0 ? primary_mid - consensus_mid : 0.0;
    p.quality = std::clamp(zone.confidence * std::clamp(window_confidence, 0.0, 1.0), 0.0, 1.0);
    return p;
}

} // namespace alttech::crossvenue
