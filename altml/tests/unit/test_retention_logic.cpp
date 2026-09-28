#include "altml/components/retention_logic.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace altml::storage;

namespace {
PolicyPressure pol(const char* name, int64_t min_keep, int64_t oldest_age) {
    PolicyPressure p;
    p.table_name = name;
    p.minimum_keep_days = min_keep;
    p.oldest_partition_age_days = oldest_age;
    return p;
}
}

TEST_CASE("a retention floor over young data blocks every reclaim", "[storage][retention]") {
    // The shape measured on this deployment: 120-day floor, ~55 days of data.
    REQUIRE(floor_blocks_reclaim(pol("altml.feature_vectors", 120, 55)));
    REQUIRE(floor_blocks_reclaim(pol("altml.labels", 120, 55)));

    // Data older than the floor is reclaimable, so the guard is not stuck.
    REQUIRE_FALSE(floor_blocks_reclaim(pol("altml.feature_vectors", 120, 121)));
    REQUIRE_FALSE(floor_blocks_reclaim(pol("altml.feature_vectors", 120, 120)));

    // A table holding nothing cannot block anything.
    REQUIRE_FALSE(floor_blocks_reclaim(pol("altml.labels", 120, -1)));
}

// Regression: the guard logged HARD_LIMIT every five minutes for days while
// retention recorded a successful run that freed nothing, and nothing in the
// system ever said the two settings were incompatible.
TEST_CASE("a hard limit that retention cannot reach is reported as a deadlock",
          "[storage][retention][regression]") {
    const std::vector<PolicyPressure> stuck = {pol("altml.feature_vectors", 120, 55),
                                               pol("altml.labels", 120, 55)};

    SECTION("over the limit, nothing freed, every policy floor-blocked") {
        REQUIRE(is_retention_deadlock(true, 0, stuck));
    }

    SECTION("not a deadlock while the guard is still freeing space") {
        REQUIRE_FALSE(is_retention_deadlock(true, 4096, stuck));
    }

    // Regression: the cycles that reported two dropped partitions freed
    // -131072 bytes. Activity is not reclaim.
    SECTION("dropping empty partitions while the database grows is still a deadlock") {
        REQUIRE(is_retention_deadlock(true, -131072, stuck));
    }

    SECTION("not a deadlock below the hard limit") {
        REQUIRE_FALSE(is_retention_deadlock(false, 0, stuck));
    }

    SECTION("not a deadlock while any policy still has reclaimable history") {
        const std::vector<PolicyPressure> mixed = {pol("altml.feature_vectors", 120, 55),
                                                   pol("altml.labels", 120, 200)};
        REQUIRE_FALSE(is_retention_deadlock(true, 0, mixed));
    }

    SECTION("no policies means nothing to conclude") {
        REQUIRE_FALSE(is_retention_deadlock(true, 0, {}));
    }

    // Regression on the fix itself, caught by deploying it and watching it stay
    // silent on a system that was visibly deadlocked.
    SECTION("an unpartitioned policy abstains instead of vetoing") {
        // altml.label_computation_runs is a plain table: partition_bounds
        // returns nothing for it, so its age comes back as -1.
        const std::vector<PolicyPressure> real = {pol("altml.feature_vectors", 120, 55),
                                                  pol("altml.labels", 120, 55),
                                                  pol("altml.label_computation_runs", 7, -1),
                                                  pol("altml.model_events", 60, -1)};
        REQUIRE(is_retention_deadlock(true, 0, real));
    }

    // Regression: empty partitions are not reclaimable history. The real
    // feature_vectors table carried 24 kB partitions at 145, 115 and 84 days
    // around the two that held 5.2 GB at 54 and 23 days.
    SECTION("age comes from the oldest POPULATED partition, not the oldest bound") {
        const std::vector<PolicyPressure> real = {pol("altml.feature_vectors", 120, 54),
                                                  pol("altml.labels", 120, 54)};
        REQUIRE(is_retention_deadlock(true, 0, real));

        // What the bug reported instead, and why it stayed silent.
        const std::vector<PolicyPressure> misread = {pol("altml.feature_vectors", 120, 145),
                                                     pol("altml.labels", 120, 145)};
        REQUIRE_FALSE(is_retention_deadlock(true, 0, misread));
    }

    SECTION("policies that all hold nothing are not a deadlock") {
        const std::vector<PolicyPressure> empty = {pol("altml.model_events", 60, -1),
                                                   pol("altml.label_computation_runs", 7, -1)};
        REQUIRE_FALSE(is_retention_deadlock(true, 0, empty));
    }
}
