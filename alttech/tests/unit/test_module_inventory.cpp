#include "alttech/application/module_inventory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <set>

TEST_CASE("service launch inventory includes every supervised analytics role") {
    const alttech::config::AnalyticsConfig configuration;
    const auto inventory=alttech::application::module_inventory(configuration);
    const std::set<std::string_view> expected={"feature","microstructure","structure","regime","cross_venue","macro"};
    std::set<std::string_view> actual;
    alttech::application::ModuleSupervisor supervisor(inventory.size());
    for(const auto& module:inventory) {
        REQUIRE(module.runner!=nullptr);REQUIRE(module.config!=nullptr);
        CHECK(actual.insert(module.name).second);
        supervisor.mark_ready(module.name);
        CHECK(supervisor.ready_count()==actual.size());
    }
    CHECK(actual==expected);
    CHECK(supervisor.wait_until_started());
}
