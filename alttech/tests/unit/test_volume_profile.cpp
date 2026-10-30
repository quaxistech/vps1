#include "volume_profile.hpp"

#include <catch2/catch_test_macros.hpp>
#include <array>
#include <limits>

TEST_CASE("profile POC and 70percent value area have deterministic ties", "[structure][profile]") {
    const auto profile = alttech::structure::profile_area(std::array<double,5>{1,4,4,1,0});
    REQUIRE(profile);
    CHECK(profile->poc == 1);
    CHECK(profile->low == 1);
    CHECK(profile->high == 2);
    CHECK(profile->total_volume == 10);
    CHECK(profile->area_volume == 8);
}

TEST_CASE("profile refuses empty invalid or unbounded volume", "[structure][profile]") {
    CHECK_FALSE(alttech::structure::profile_area(std::array<double,3>{0,0,0}));
    CHECK_FALSE(alttech::structure::profile_area(std::array<double,3>{1,-1,1}));
    CHECK_FALSE(alttech::structure::profile_area(std::array<double,3>{1,std::numeric_limits<double>::infinity(),1}));
    const auto all = alttech::structure::profile_area(std::array<double,3>{2,1,1},1.0);
    REQUIRE(all);
    CHECK(all->area_volume == all->total_volume);
    CHECK(all->low == 0);
    CHECK(all->high == 2);
}
