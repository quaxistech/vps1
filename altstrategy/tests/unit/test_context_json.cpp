// decision_context_snapshots.snapshot is the point-in-time record downstream
// consumers reconstruct the state at T0 from. Two encoding mistakes here are
// silent until they are weeks old: a non-finite double streams as `nan` and
// aborts the whole INSERT, and the default 6 significant digits truncate a
// price into a number the decision was never taken on.

#include "altstrategy/engine_io/context_json.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace altstrategy::engine_io;
using namespace altstrategy::engine;

TEST_CASE("a price keeps every digit it was decided on") {
    const std::string json = json_of({{"mark_price", 43567.89}});
    // The default ostream precision would render this as 43567.9.
    REQUIRE(json.find("43567.89") != std::string::npos);
}

TEST_CASE("non-finite values are omitted, never streamed as nan") {
    const std::string json =
        json_of({{"good", 1.5},
                 {"nan", std::nan("")},
                 {"inf", std::numeric_limits<double>::infinity()}});
    REQUIRE(json.find("nan") == std::string::npos);
    REQUIRE(json.find("inf") == std::string::npos);
    REQUIRE(json.find("\"good\":1.5") != std::string::npos);
    // A value that is not a number must be ABSENT downstream, not substituted.
    REQUIRE(json.find("\"nan\"") == std::string::npos);
}

TEST_CASE("an empty map is a valid empty object") {
    REQUIRE(json_of(std::map<std::string, double>{}) == "{}");
}

TEST_CASE("the snapshot carries every analytics block the strategies read") {
    DecisionContext c;
    c.bitget_features.values = {{"atr_14", 1.5}, {"rsi_14", 61.0}};
    c.microstructure.values = {{"book_imbalance", 0.25}};
    c.cross_venue_features.values = {{"dislocation_bps", 3.5}};
    // Two information venues, each keeping its own reading: Binance (2) and
    // Bybit (4). The export must say whose number is whose.
    c.reference_features[2].values = {{"rsi_14", 55.0}};
    c.reference_features[4].values = {{"rsi_14", 48.0}};
    c.bitget_regime.regime = "TREND_UP";
    c.bitget_regime.confidence = 0.8;
    c.global_regime.regime = "RANGE";
    c.global_regime.trend_strength = 0.1;

    const std::string json = context_snapshot_json(c);
    // Venue identity survives serialisation: a flat merge would lose which
    // market each reading came from.
    CHECK(json.find("\"2\":") != std::string::npos);
    CHECK(json.find("\"4\":") != std::string::npos);
    CHECK(json.find("55") != std::string::npos);
    CHECK(json.find("48") != std::string::npos);
    for (const char* key : {"bitget_features", "microstructure", "cross_venue_features",
                            "reference_features", "bitget_regime_view", "global_regime_view"}) {
        INFO(key);
        REQUIRE(json.find(std::string("\"") + key + "\":") != std::string::npos);
    }
    REQUIRE(json.find("\"atr_14\":1.5") != std::string::npos);
    REQUIRE(json.find("\"regime\":\"TREND_UP\"") != std::string::npos);
    REQUIRE(json.find("\"regime\":\"RANGE\"") != std::string::npos);
    REQUIRE(json.find("\"confidence\":0.8") != std::string::npos);
    REQUIRE(json.front() == '{');
    REQUIRE(json.back() == '}');
}

TEST_CASE("an empty context still produces a valid, complete object") {
    // A context assembled before any analytics arrived must not produce
    // malformed JSON that aborts the snapshot INSERT.
    const std::string json = context_snapshot_json(DecisionContext{});
    REQUIRE(json.find("\"bitget_features\":{}") != std::string::npos);
    REQUIRE(json.find("\"global_regime_view\":{\"regime\":\"UNDEFINED\"") != std::string::npos);
}
