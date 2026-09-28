#pragma once
// Cross-venue aggregation of the per-venue regimes and structure zones.
//
// Pure: it takes the per-venue results that the venue sweeps already produced
// plus the consensus weights computed for the same bar, and returns the global
// regime and the consensus zone set. No PostgreSQL, NATS or protobuf.

#include "analytics_types.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace alttech::crossvenue {

// One venue's regime for the bar being aggregated, as stored by the venue
// sweep. `weight` is the same quality/liquidity weight the price consensus
// used, so an illiquid or stale venue contributes proportionally less here too.
struct VenueRegimeInput {
    Venue venue = Venue::unknown;
    std::string regime = "UNDEFINED";
    double confidence = 0.0;
    double weight = 0.0;
    double trend_strength = 0.0;
    double volatility_regime = 0.0;
    double liquidity_quality = 0.0;
};

struct GlobalRegimeResult {
    bool available = false;
    std::string primary_regime = "UNDEFINED";
    double regime_confidence = 0.0;
    // Share of the participating weight that voted for the winning regime.
    double venue_agreement = 0.0;
    // Normalized Shannon entropy of the weighted regime distribution:
    // 0 when the venues are unanimous, 1 when they are maximally split.
    double venue_dispersion = 0.0;
    uint32_t supporting_venue_mask = 0;
    bool bitget_divergence = false;
    double trend_consensus = 0.0;
    double volatility_consensus = 0.0;
    double liquidity_consensus = 0.0;
};

[[nodiscard]] GlobalRegimeResult aggregate_regimes(std::span<const VenueRegimeInput> inputs);

// One venue's structure zone, in that venue's own prices.
struct VenueZoneInput {
    Venue venue = Venue::unknown;
    std::string kind;    // support | resistance | dynamic
    double lower = 0.0;
    double center = 0.0;
    double upper = 0.0;
    double strength = 0.0;
    double venue_mid = 0.0; // that venue's mid price for the same bar
    double weight = 0.0;
};

// A zone the venues agree on, expressed relative to each venue's own mid price
// so that the per-venue basis is removed and the coordinate is comparable.
struct ConsensusZone {
    std::string kind;
    double normalized_center = 0.0; // (zone center / venue mid) - 1, weighted
    double normalized_width = 0.0;  // full band width, relative to mid
    uint32_t supporting_venue_mask = 0;
    int supporting_venue_count = 0;
    double venue_confluence_score = 0.0; // share of total weight supporting it
    double strength = 0.0;
    double confidence = 0.0;
};

// Cluster per-venue zones into consensus zones.
//
// Two zones belong to the same cluster when their normalized bands overlap.
// That is a parameter-free geometric criterion: the venues literally point at
// the same price region once their basis is removed. A cluster becomes a
// consensus zone only when at least `min_venues` distinct venues support it.
[[nodiscard]] std::vector<ConsensusZone> build_consensus_zones(
    std::span<const VenueZoneInput> zones, int min_venues = 2);

// Project a consensus zone back onto the primary venue's price scale.
struct ProjectedZone {
    double lower = 0.0;
    double center = 0.0;
    double upper = 0.0;
    double basis_adjustment = 0.0; // primary mid minus consensus mid
    double quality = 0.0;          // [0,1]
};

[[nodiscard]] ProjectedZone project_to_primary(const ConsensusZone& zone, double primary_mid,
                                               double consensus_mid, double window_confidence);

} // namespace alttech::crossvenue
