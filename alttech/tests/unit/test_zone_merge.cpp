// Confluence-weighted merging of structure-zone candidates.
//
// The regression this guards: the merge weight was computed and then discarded
// with `(void)wsum`, so the centre was a plain average of the two candidates. A
// level confirmed by five methods merged with a single-method level as an
// equal, and the resulting centre landed halfway between them instead of near
// the stronger one. AltStrategy reads these centres as price levels, so the
// error moved trade levels.

#include "zone_merge.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using alttech::structure::merge_zone_candidates;
using alttech::structure::zone_weight;
using alttech::structure::ZoneCandidate;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

ZoneCandidate zone(double center, const char* kind,
                   std::map<std::string, double> methods) {
    ZoneCandidate z;
    z.center = center;
    z.lower = center - 1.0;
    z.upper = center + 1.0;
    z.kind = kind;
    z.methods = std::move(methods);
    return z;
}

} // namespace

TEST_CASE("a strongly confirmed zone dominates the merged centre") {
    // 100.0 confirmed by four methods (total weight 4.0) against 102.0 with one
    // (weight 1.0). A plain average would give 101.0; weighting gives 100.4.
    std::vector<ZoneCandidate> in{
        zone(100.0, "support",
             {{"swing", 1.0}, {"donchian20_low", 1.0}, {"donchian55_low", 1.2}, {"vwap", 0.8}}),
        zone(102.0, "support", {{"round_number", 1.0}}),
    };
    const auto out = merge_zone_candidates(in, 5.0);
    REQUIRE(out.size() == 1);
    CHECK_THAT(out[0].center, WithinRel(100.4, 1e-9));
    CHECK(out[0].center < 101.0);   // the defect produced exactly 101.0
}

TEST_CASE("equal confluence merges to the midpoint") {
    std::vector<ZoneCandidate> in{
        zone(100.0, "resistance", {{"swing", 1.0}}),
        zone(102.0, "resistance", {{"round_number", 1.0}}),
    };
    const auto out = merge_zone_candidates(in, 5.0);
    REQUIRE(out.size() == 1);
    CHECK_THAT(out[0].center, WithinAbs(101.0, 1e-9));
}

TEST_CASE("merging respects kind and distance") {
    SECTION("different kinds never merge") {
        std::vector<ZoneCandidate> in{
            zone(100.0, "support", {{"swing", 1.0}}),
            zone(100.5, "resistance", {{"swing", 1.0}}),
        };
        CHECK(merge_zone_candidates(in, 5.0).size() == 2);
    }
    SECTION("candidates further apart than half a width stay separate") {
        std::vector<ZoneCandidate> in{
            zone(100.0, "support", {{"swing", 1.0}}),
            zone(120.0, "support", {{"swing", 1.0}}),
        };
        CHECK(merge_zone_candidates(in, 5.0).size() == 2);
    }
}

TEST_CASE("merging accumulates evidence rather than replacing it") {
    std::vector<ZoneCandidate> a{zone(100.0, "support", {{"swing", 1.0}})};
    a[0].touch_count = 3;
    a[0].volume_weight = 500.0;
    ZoneCandidate b = zone(100.4, "support", {{"donchian20_low", 1.0}});
    b.touch_count = 2;
    b.volume_weight = 300.0;
    a.push_back(b);

    const auto out = merge_zone_candidates(a, 5.0);
    REQUIRE(out.size() == 1);
    CHECK(out[0].methods.size() == 2);          // both methods survive
    CHECK(out[0].touch_count == 5);             // touches add
    CHECK_THAT(out[0].volume_weight, WithinAbs(800.0, 1e-9));
    // Bounds follow the merged centre, not the original candidate.
    CHECK_THAT(out[0].lower, WithinAbs(out[0].center - 5.0, 1e-9));
    CHECK_THAT(out[0].upper, WithinAbs(out[0].center + 5.0, 1e-9));
}

TEST_CASE("a candidate with no recorded method still participates") {
    std::vector<ZoneCandidate> in{
        zone(100.0, "support", {}),                       // weight falls back to 1.0
        zone(101.0, "support", {{"swing", 3.0}}),
    };
    const auto out = merge_zone_candidates(in, 5.0);
    REQUIRE(out.size() == 1);
    CHECK_THAT(zone_weight(in[0]), WithinAbs(1.0, 1e-9));
    CHECK_THAT(out[0].center, WithinRel(100.75, 1e-9));   // (100*1 + 101*3) / 4
}
