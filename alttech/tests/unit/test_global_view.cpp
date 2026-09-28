// Cross-venue aggregation: global regime vote and consensus zone clustering.
#include "global_view.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

using namespace alttech;
using namespace alttech::crossvenue;
using Catch::Matchers::WithinAbs;

namespace {

VenueRegimeInput regime(Venue venue, const char* label, double weight, double confidence = 0.8) {
    VenueRegimeInput in;
    in.venue = venue;
    in.regime = label;
    in.weight = weight;
    in.confidence = confidence;
    return in;
}

VenueZoneInput zone(Venue venue, const char* kind, double center, double half, double mid,
                    double weight = 1.0, double strength = 0.5) {
    VenueZoneInput z;
    z.venue = venue;
    z.kind = kind;
    z.center = center;
    z.lower = center - half;
    z.upper = center + half;
    z.venue_mid = mid;
    z.weight = weight;
    z.strength = strength;
    return z;
}

} // namespace

TEST_CASE("unanimous venues give full agreement and zero dispersion") {
    const std::vector<VenueRegimeInput> inputs = {
        regime(Venue::bitget, "TREND_UP", 1.0), regime(Venue::binance_usdm, "TREND_UP", 1.0),
        regime(Venue::okx, "TREND_UP", 1.0), regime(Venue::bybit, "TREND_UP", 1.0)};
    const auto r = aggregate_regimes(inputs);
    REQUIRE(r.available);
    CHECK(r.primary_regime == "TREND_UP");
    CHECK_THAT(r.venue_agreement, WithinAbs(1.0, 1e-9));
    CHECK_THAT(r.venue_dispersion, WithinAbs(0.0, 1e-9));
    CHECK(r.supporting_venue_mask == 0b1111U);
    CHECK_FALSE(r.bitget_divergence);
}

TEST_CASE("an evenly split vote is maximally dispersed") {
    const std::vector<VenueRegimeInput> inputs = {
        regime(Venue::bitget, "TREND_UP", 1.0), regime(Venue::binance_usdm, "TREND_UP", 1.0),
        regime(Venue::okx, "RANGE", 1.0), regime(Venue::bybit, "RANGE", 1.0)};
    const auto r = aggregate_regimes(inputs);
    REQUIRE(r.available);
    CHECK_THAT(r.venue_agreement, WithinAbs(0.5, 1e-9));
    CHECK_THAT(r.venue_dispersion, WithinAbs(1.0, 1e-9));
}

TEST_CASE("a low-weight venue cannot outvote the liquid ones") {
    const std::vector<VenueRegimeInput> inputs = {
        regime(Venue::bitget, "RANGE", 1.0), regime(Venue::binance_usdm, "RANGE", 1.0),
        regime(Venue::okx, "CHAOTIC", 0.01), regime(Venue::bybit, "CHAOTIC", 0.01)};
    const auto r = aggregate_regimes(inputs);
    REQUIRE(r.available);
    CHECK(r.primary_regime == "RANGE");
    CHECK(r.venue_agreement > 0.9);
}

TEST_CASE("the primary venue diverging from the consensus is reported") {
    const std::vector<VenueRegimeInput> inputs = {
        regime(Venue::bitget, "BREAKOUT", 1.0), regime(Venue::binance_usdm, "RANGE", 1.0),
        regime(Venue::okx, "RANGE", 1.0), regime(Venue::bybit, "RANGE", 1.0)};
    const auto r = aggregate_regimes(inputs);
    REQUIRE(r.available);
    CHECK(r.primary_regime == "RANGE");
    CHECK(r.bitget_divergence);
}

TEST_CASE("UNDEFINED venue regimes do not enter the vote") {
    const std::vector<VenueRegimeInput> inputs = {
        regime(Venue::bitget, "UNDEFINED", 1.0), regime(Venue::binance_usdm, "UNDEFINED", 1.0)};
    CHECK_FALSE(aggregate_regimes(inputs).available);
}

TEST_CASE("zones agreeing across venues cluster once their basis is removed") {
    // Same relative level (+1%) on three venues whose absolute prices differ.
    const std::vector<VenueZoneInput> zones = {
        zone(Venue::bitget, "resistance", 101.0, 0.1, 100.0),
        zone(Venue::binance_usdm, "resistance", 202.0, 0.2, 200.0),
        zone(Venue::okx, "resistance", 50.5, 0.05, 50.0)};
    const auto out = build_consensus_zones(zones, 2);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == "resistance");
    CHECK_THAT(out[0].normalized_center, WithinAbs(0.01, 1e-6));
    CHECK(out[0].supporting_venue_count == 3);
    CHECK(out[0].supporting_venue_mask == 0b0111U);
}

TEST_CASE("a level only one venue sees is not a consensus zone") {
    const std::vector<VenueZoneInput> zones = {
        zone(Venue::bitget, "support", 99.0, 0.05, 100.0),
        zone(Venue::binance_usdm, "support", 190.0, 0.1, 200.0)}; // -5%, far away
    CHECK(build_consensus_zones(zones, 2).empty());
}

TEST_CASE("support and resistance never merge into one zone") {
    const std::vector<VenueZoneInput> zones = {
        zone(Venue::bitget, "support", 101.0, 0.5, 100.0),
        zone(Venue::binance_usdm, "resistance", 202.0, 1.0, 200.0)};
    CHECK(build_consensus_zones(zones, 2).empty());
}

TEST_CASE("projection carries the consensus level onto the primary venue's prices") {
    ConsensusZone z;
    z.kind = "resistance";
    z.normalized_center = 0.01; // 1% above mid
    z.normalized_width = 0.002; // 20 bps wide
    z.confidence = 0.8;
    const auto p = project_to_primary(z, 100.0, 99.0, 1.0);
    CHECK_THAT(p.center, WithinAbs(101.0, 1e-9));
    CHECK_THAT(p.lower, WithinAbs(100.9, 1e-9));
    CHECK_THAT(p.upper, WithinAbs(101.1, 1e-9));
    CHECK_THAT(p.basis_adjustment, WithinAbs(1.0, 1e-9));
    CHECK_THAT(p.quality, WithinAbs(0.8, 1e-9));
}

TEST_CASE("a degraded window lowers the projection quality") {
    ConsensusZone z;
    z.kind = "support";
    z.normalized_center = -0.005;
    z.normalized_width = 0.001;
    z.confidence = 0.8;
    const auto p = project_to_primary(z, 100.0, 100.0, 0.5);
    CHECK_THAT(p.quality, WithinAbs(0.4, 1e-9));
}
