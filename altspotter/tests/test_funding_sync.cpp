// Regression cover for the funding-settlement sync.
//
// The defect: when the stored history was shallower than funding_backfill_days,
// the hourly pass fetched only the OLDER slice. Bitget keeps ~90 days of
// settlement history against a configured 180, so that request returned an
// empty page every hour and the forward catch-up -- in another branch -- never
// ran: Bitget and Bybit received no settlement after 2026-08-31 04:00.

#include "components/funding_sync.hpp"

#include <catch2/catch_test_macros.hpp>

using altspotter::funding::FetchRange;
using altspotter::funding::plan_fetches;

namespace {
constexpr int64_t kHour = 3'600'000;
constexpr int64_t kDay = 24 * kHour;
// 2026-09-24T12:00:00Z
constexpr int64_t kNow = 1790251200000LL;
constexpr int64_t kTarget = kNow - 180 * kDay;   // funding_backfill_days: 180
} // namespace

TEST_CASE("a venue with a shorter history than configured still gets new settlements",
          "[funding][regression]") {
    // Bitget on VPS1: oldest row ~114 days back, newest 24 days back.
    const int64_t oldest = kNow - 114 * kDay;
    const int64_t newest = kNow - 24 * kDay;
    const auto plan = plan_fetches(newest, oldest, kNow, kTarget, false);
    REQUIRE(plan.size() == 2);
    // Forward first: new data never waits behind the deepening.
    REQUIRE_FALSE(plan[0].deepen);
    REQUIRE(plan[0].from_ms == newest);
    REQUIRE(plan[0].to_ms == kNow);
    REQUIRE(plan[1].deepen);
    REQUIRE(plan[1].from_ms == kTarget);
    REQUIRE(plan[1].to_ms == oldest - 1);
}

TEST_CASE("once the venue has shown it has nothing older, only the forward range is asked",
          "[funding]") {
    const int64_t oldest = kNow - 114 * kDay;
    const int64_t newest = kNow - 5 * kHour;
    const auto plan = plan_fetches(newest, oldest, kNow, kTarget, true);
    REQUIRE(plan.size() == 1);
    REQUIRE_FALSE(plan[0].deepen);
    REQUIRE(plan[0].from_ms == newest);
}

TEST_CASE("nothing stored loads the whole configured window", "[funding]") {
    const auto plan = plan_fetches(0, 0, kNow, kTarget, false);
    REQUIRE(plan.size() == 1);
    REQUIRE(plan[0].from_ms == kTarget);
    REQUIRE(plan[0].to_ms == kNow);
    REQUIRE_FALSE(plan[0].deepen);
}

TEST_CASE("a deep enough history is only caught up forward", "[funding]") {
    // Binance/OKX on VPS1: history reaches past the target.
    const int64_t oldest = kTarget - 20 * kDay;
    const int64_t newest = kNow - 4 * kHour;
    const auto plan = plan_fetches(newest, oldest, kNow, kTarget, false);
    REQUIRE(plan.size() == 1);
    REQUIRE_FALSE(plan[0].deepen);
}

TEST_CASE("an up-to-date instrument with its full depth asks for nothing", "[funding]") {
    const int64_t oldest = kTarget;
    const int64_t newest = kNow - 10 * 60'000;   // ten minutes ago
    REQUIRE(plan_fetches(newest, oldest, kNow, kTarget, false).empty());
    // The one-day slack keeps a depth that is only a few hours short from
    // re-asking every hour as the target moves with the clock.
    REQUIRE(plan_fetches(newest, kTarget + 2 * kHour, kNow, kTarget, false).empty());
}
