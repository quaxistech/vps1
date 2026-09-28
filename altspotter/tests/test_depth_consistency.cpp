// Regression tests for depth-vs-retention consistency.
//
// Each rule here corresponds to a measured incident on VPS1, where data was
// fetched from a venue, written, and then deleted by a component whose
// retention had not been raised alongside the depth setting. The failures were
// silent: the backfill logged success while retention removed its output.
//
// These call the SAME function `config-check` calls. A rule cannot pass
// validation and be untested here.

#include "ops/depth_consistency.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using altspotter::ops::DepthSettings;
using altspotter::ops::depth_violations;
using altspotter::ops::projected_bytes;

namespace {

// The configuration VPS1 actually runs, as of release 20260831-063110.
DepthSettings production() {
    DepthSettings d;
    d.bootstrap_days = 280;
    d.candles_1m_days = 280;
    d.candles_agg_days = 400;
    d.mark_index_backfill_days = 90;
    d.derivatives_context_days = 90;
    d.funding_backfill_days = 180;
    d.funding_settlements_days = 200;
    d.raw_trades_core_hours = 60;
    d.raw_trades_other_hours = 24;
    d.hard_database_limit_bytes = 68'719'476'736; // 64 GiB
    d.cleanup_ratio = 0.87;
    return d;
}

bool mentions(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v) {
        if (s.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST_CASE("the deployed configuration is self-consistent", "[config][retention]") {
    REQUIRE(depth_violations(production()).empty());
}

TEST_CASE("a bootstrap deeper than 1m retention is rejected", "[config][retention]") {
    // The original incident: bootstrap_days 90 against candles_1m_days 60.
    // Retention run 76 dropped 11 partitions and 630 MB mid-backfill, and 2318
    // rows were rejected because their partitions had just been deleted.
    auto d = production();
    d.bootstrap_days = 90;
    d.candles_1m_days = 60;
    const auto v = depth_violations(d);
    REQUIRE_FALSE(v.empty());
    REQUIRE(mentions(v, "bootstrap_days (90) > candles_1m_days (60)"));

    SECTION("equal depths are allowed") {
        d.bootstrap_days = 60;
        REQUIRE_FALSE(mentions(depth_violations(d), "bootstrap_days"));
    }
}

TEST_CASE("mark/index depth must fit the derivatives retention", "[config][retention]") {
    auto d = production();
    d.mark_index_backfill_days = 280;
    REQUIRE(mentions(depth_violations(d), "mark_index_backfill_days (280)"));
}

TEST_CASE("funding depth must fit its own retention block", "[config][retention]") {
    // funding_backfill_days lives in the venue runtime, funding_settlements_days
    // in ops_retention -- two different blocks, one decision. At 180 vs 90,
    // Binance and Bybit were truncated back to 90 days on the next pass,
    // silently undoing the deepening that had just been fixed.
    auto d = production();
    d.funding_settlements_days = 90;
    REQUIRE(mentions(depth_violations(d), "funding_backfill_days (180) > funding_settlements_days (90)"));
}

TEST_CASE("the storage gate must hold what retention keeps", "[config][storage]") {
    auto d = production();

    SECTION("the pre-incident 24 GiB limit is refused") {
        // Sized when retention kept 14 days and the database settled near 9 GB.
        // Against 280-day candles it sheds at 87% = 21 GB, mid-backfill.
        d.hard_database_limit_bytes = 25'769'803'776; // 24 GiB
        REQUIRE(mentions(depth_violations(d), "hard_database_limit_bytes too small"));
    }

    SECTION("the projection tracks the retention that drives it") {
        const double before = projected_bytes(d);
        d.candles_1m_days *= 2;
        REQUIRE(projected_bytes(d) > before);
    }

    SECTION("raw-trade hours are part of the projection") {
        const double before = projected_bytes(d);
        d.raw_trades_core_hours = 6; // the old window
        REQUIRE(projected_bytes(d) < before);
    }
}

TEST_CASE("violations are reported together, not one at a time", "[config]") {
    // An operator fixing a config should see every contradiction in one pass.
    DepthSettings d;
    d.bootstrap_days = 400;
    d.candles_1m_days = 60;
    d.mark_index_backfill_days = 200;
    d.derivatives_context_days = 14;
    d.funding_backfill_days = 180;
    d.funding_settlements_days = 30;
    d.hard_database_limit_bytes = 1'073'741'824; // 1 GiB
    d.candles_agg_days = 400;
    d.raw_trades_core_hours = 60;
    d.raw_trades_other_hours = 24;
    REQUIRE(depth_violations(d).size() == 4);
}
