#include "components/gap_repair_logic.hpp"
#include "test_env.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

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

// Database-backed regression for the October 2026 incident. This uses the
// harness's ISOLATED, temporary PostgreSQL database, never the VPS1 database.
TEST_CASE("repair guards a retention-dropped month", "[integration][gap][regression]") {
    if (!altspotter::test::TempDb::admin_dsn()) {
        SKIP("isolated PostgreSQL test DSN unavailable (ALTSPOTTER_TEST_PG_DSN)");
    }
    altspotter::test::TempDb db;
    REQUIRE(db.create());
    const std::string bin = altspotter::test::bin_dir() + "/altspotter";
    REQUIRE(altspotter::test::run_tool(
        bin, {"migrate", "--dsn", db.dsn(), "--migrations", altspotter::test::migrations_dir()}) == 0);

    pqxx::connection conn(db.dsn());
    const auto exec = [&](const char* sql) {
        pqxx::work tx(conn);
        tx.exec(sql);
        tx.commit();
    };
    const auto covered = [&](int64_t from_ms, int64_t to_ms) {
        pqxx::read_transaction tx(conn);
        const auto r = tx.exec_params1(
            std::string(altspotter::integrity::kRepairPartitionsPresentSql), from_ms, to_ms);
        return r[0].as<bool>();
    };
    // The migration helper creates partitions using session timezone for
    // DATE -> timestamptz casts. Set UTC before creating fixture partitions,
    // so the fixture models true UTC calendar months on any developer host.
    exec("SET TIME ZONE 'UTC'");
    // April and June are present; the gap month May has been dropped.
    exec("SELECT altspotter_ensure_candle_partitions("
         "'2026-04-01 00:00:00+00'::timestamptz, '2026-06-01 00:00:00+00'::timestamptz)");
    exec("DROP TABLE IF EXISTS market_candles_1m_202605");

    constexpr int64_t apr30 = 1777593540000LL; // 2026-04-30 23:59 UTC
    constexpr int64_t may1  = 1777593600000LL; // 2026-05-01 00:00 UTC
    constexpr int64_t june1 = 1780272000000LL; // 2026-06-01 00:00 UTC
    CHECK(covered(apr30, apr30));
    CHECK_FALSE(covered(may1, may1));
    CHECK_FALSE(covered(apr30, june1));
    CHECK(covered(june1, june1));
    // A partition that covers only part of a calendar month is not safe
    // for a full-month repair. A probe of only the month start would miss it.
    exec("CREATE TABLE market_candles_1m_202605 PARTITION OF market_candles_1m "
         "FOR VALUES FROM ('2026-05-01 00:00:00+00') TO ('2026-05-31 21:00:00+00')");
    CHECK_FALSE(covered(may1, may1));
    exec("DROP TABLE market_candles_1m_202605");
    // Explicitly providing capacity/partition makes this range eligible again.
    exec("SELECT altspotter_ensure_candle_partitions("
         "'2026-05-01 00:00:00+00'::timestamptz, '2026-05-01 00:00:00+00'::timestamptz)");
    CHECK(covered(may1, may1));
    CHECK(covered(apr30, june1));
    // The read-only guard itself must not depend on the caller's timezone.
    exec("SET TIME ZONE 'Europe/Moscow'");
    CHECK(covered(may1, may1));
    CHECK(covered(apr30, june1));
}


// Regression: the creator's DATE -> timestamptz casts previously depended on
// the backend timezone and could produce a month shifted by several hours.
// Uses only the harness's disposable PostgreSQL database.
TEST_CASE("UTC candle partitions are independent of session timezone",
          "[integration][partition][regression]") {
    if (!altspotter::test::TempDb::admin_dsn()) {
        SKIP("isolated PostgreSQL test DSN unavailable (ALTSPOTTER_TEST_PG_DSN)");
    }
    altspotter::test::TempDb db;
    REQUIRE(db.create());
    const std::string bin = altspotter::test::bin_dir() + "/altspotter";
    REQUIRE(altspotter::test::run_tool(
        bin, {"migrate", "--dsn", db.dsn(), "--migrations",
              altspotter::test::migrations_dir()}) == 0);

    pqxx::connection conn(db.dsn());
    const auto exec = [&](const char* sql) {
        pqxx::work tx(conn);
        tx.exec(sql);
        tx.commit();
    };
    const auto exact = [&](const std::string& parent, const std::string& child,
                           const char* lower, const char* upper) {
        pqxx::read_transaction tx(conn);
        const auto row = tx.exec_params1(
            "SELECT EXISTS(SELECT 1 FROM altspotter_partition_bounds($1::regclass) b "
                "WHERE b.partition_name = $2 "
                "AND b.lower_bound = $3::timestamptz "
                "AND b.upper_bound = $4::timestamptz)",
            parent, child, lower, upper);
        return row[0].as<bool>();
    };

    exec("SET TIME ZONE 'Europe/Moscow'");
    exec("SELECT altspotter_ensure_candle_partitions("
         "'2026-03-01 00:00:00+00'::timestamptz, "
         "'2026-03-01 00:00:00+00'::timestamptz)");
    CHECK(exact("market_candles_1m", "market_candles_1m_202603",
                "2026-03-01 00:00:00+00", "2026-04-01 00:00:00+00"));
    CHECK(exact("market_candles_agg", "market_candles_agg_202603",
                "2026-03-01 00:00:00+00", "2026-04-01 00:00:00+00"));

    exec("SET TIME ZONE 'America/New_York'");
    exec("SELECT altspotter_ensure_candle_partitions("
         "'2026-02-01 00:00:00+00'::timestamptz, "
         "'2026-02-01 00:00:00+00'::timestamptz)");
    CHECK(exact("market_candles_1m", "market_candles_1m_202602",
                "2026-02-01 00:00:00+00", "2026-03-01 00:00:00+00"));
    CHECK(exact("market_candles_agg", "market_candles_agg_202602",
                "2026-02-01 00:00:00+00", "2026-03-01 00:00:00+00"));

    // Legacy partition with local-time bounds must not be silently treated
    // as a valid UTC month or be dropped/recreated by the migration helper.
    exec("SET TIME ZONE 'Europe/Moscow'");
    exec("CREATE TABLE market_candles_1m_202601 PARTITION OF market_candles_1m "
         "FOR VALUES FROM ('2026-01-01'::timestamptz) "
         "TO ('2026-02-01'::timestamptz)");
    CHECK_FALSE(exact("market_candles_1m", "market_candles_1m_202601",
                      "2026-01-01 00:00:00+00", "2026-02-01 00:00:00+00"));
    CHECK_THROWS([&] {
        pqxx::work tx(conn);
        tx.exec("SELECT altspotter_ensure_candle_partitions("
                "'2026-01-01 00:00:00+00'::timestamptz, "
                "'2026-01-01 00:00:00+00'::timestamptz)");
        tx.commit();
    }());
    CHECK_FALSE(exact("market_candles_1m", "market_candles_1m_202601",
                      "2026-01-01 00:00:00+00", "2026-02-01 00:00:00+00"));
}
