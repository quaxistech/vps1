#include "components/gap_repair_logic.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace altspotter::integrity;

namespace {
constexpr int64_t kMin = kMsPerMinute;
}

// Regression: a gap longer than one command used to re-request its own first
// chunk on every dispatch. The window has to advance with the cursor, or the
// tail of a long gap is never requested at all.
TEST_CASE("a long gap is covered chunk by chunk", "[integrity][gap][regression]") {
    // The shape that produced 510 unrepairable gaps in production: 4105 minutes
    // of missing candles, 1000 minutes per command, 5 attempts.
    constexpr int64_t kStart = 1'000'000'000'000;
    constexpr int64_t kGapMinutes = 4105;
    constexpr int64_t kChunk = 1000;
    const int64_t gap_end = kStart + (kGapMinutes - 1) * kMin;

    SECTION("a window that starts at gap_start covers only its head") {
        const int64_t end = repair_window_end(kStart, gap_end, kChunk);
        REQUIRE(end == kStart + (kChunk - 1) * kMin);
        REQUIRE(end < gap_end);   // the rest is untouched by this command
    }

    SECTION("advancing the cursor eventually covers the whole gap") {
        int64_t cursor = kStart;
        int dispatches = 0;
        while (cursor <= gap_end) {
            cursor = repair_window_end(cursor, gap_end, kChunk) + kMin;
            ++dispatches;
            REQUIRE(dispatches < 100);   // must terminate
        }
        REQUIRE(dispatches == 5);        // ceil(4105 / 1000)
    }

    SECTION("the last window is clamped to the end of the gap") {
        const int64_t last_start = kStart + 4000 * kMin;
        REQUIRE(repair_window_end(last_start, gap_end, kChunk) == gap_end);
    }
}

// Regression: `attempts` counted dispatches, so a gap was condemned for being
// long rather than for being unrecoverable.
TEST_CASE("the attempt budget measures being stuck, not being long",
          "[integrity][gap][regression]") {
    SECTION("progress restarts the budget") {
        REQUIRE(next_attempt_number(true, 4) == 1);
        REQUIRE(next_attempt_number(true, 0) == 1);
    }

    SECTION("no progress spends it") {
        REQUIRE(next_attempt_number(false, 0) == 1);
        REQUIRE(next_attempt_number(false, 4) == 5);
    }

    SECTION("a gap that keeps recovering data is never retired") {
        constexpr int kMaxAttempts = 5;
        int attempts = 0;
        for (int dispatch = 0; dispatch < 50; ++dispatch) {
            attempts = next_attempt_number(true, attempts);
            REQUIRE(attempts < kMaxAttempts);
        }
    }

    SECTION("a genuinely stuck gap still retires") {
        constexpr int kMaxAttempts = 5;
        int attempts = 0;
        int dispatches = 0;
        while (attempts < kMaxAttempts) {
            attempts = next_attempt_number(false, attempts);
            ++dispatches;
        }
        REQUIRE(dispatches == kMaxAttempts);
    }
}

TEST_CASE("backoff follows progress", "[integrity][gap]") {
    constexpr int64_t kBase = 120;
    constexpr int64_t kMax = 7200;

    REQUIRE(backoff_ceiling_sec(true, 4, kBase, kMax) == kBase);
    REQUIRE(backoff_ceiling_sec(false, 0, kBase, kMax) == kBase);
    REQUIRE(backoff_ceiling_sec(false, 3, kBase, kMax) == kBase * 8);
    REQUIRE(backoff_ceiling_sec(false, 10, kBase, kMax) == kMax);   // clamped
}

// Regression: once attempts can restart, gap id + attempt repeats, and
// JetStream would drop the new command as a duplicate.
TEST_CASE("a repair command is identified by its window", "[integrity][gap][regression]") {
    constexpr int64_t kStart = 1'000'000'000'000;
    const int64_t second_window = kStart + 1000 * kMin;

    // Same gap, same attempt number after a reset, different window.
    REQUIRE(repair_message_id(7, kStart, 1) != repair_message_id(7, second_window, 1));
    REQUIRE(repair_message_id(7, kStart, 1) == repair_message_id(7, kStart, 1));
    REQUIRE(repair_message_id(7, kStart, 1) != repair_message_id(8, kStart, 1));
    REQUIRE(repair_message_id(7, kStart, 1) != repair_message_id(7, kStart, 2));
}
