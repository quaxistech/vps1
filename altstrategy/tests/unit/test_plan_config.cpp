#include "altstrategy/engine_io/plan_config.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace altstrategy;

TEST_CASE("configured holding hours reject invalid original integers before multiplication") {
    for (const auto* value : {"0","-1","49","9223372036854775807","-9223372036854775808",
                              "9223372036854775808","99999999999999999999999999999",
                              "1.5","true","[]","null"}) {
        INFO(value);
        const auto config=YAML::Load(std::string("max_holding_hours: ")+value);
        CHECK_FALSE(engine_io::parse_max_holding_ms(config));
    }
    const auto minimum=engine_io::parse_max_holding_ms(YAML::Load("max_holding_hours: 1"));
    REQUIRE(minimum);CHECK(*minimum==3'600'000);
    const auto maximum=engine_io::parse_max_holding_ms(YAML::Load("max_holding_hours: 48"));
    REQUIRE(maximum);CHECK(*maximum==engine::kMaxHoldingMs);
    const auto absent=engine_io::parse_max_holding_ms(YAML::Load("{}"));
    REQUIRE(absent);CHECK(*absent==engine::kMaxHoldingMs);
}
