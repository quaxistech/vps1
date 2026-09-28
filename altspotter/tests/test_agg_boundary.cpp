// Regression cover for the aggregate partition floor.
//
// The defect: market_candles_1m and market_candles_agg are partitioned by month
// with different retention horizons, so a month can keep its 1m partition after
// its aggregate partition is dropped. Catch-up saw complete 1m windows with no
// aggregate row, published them, and the writer could not insert them ("no
// partition of relation found for row"). The writer then isolated every batch
// one message at a time, which delayed live candles past the downstream
// staleness budget and stalled the pipeline for six days.
//
// These tests drive the statements the service actually issues (the constants
// in agg_boundary.hpp) against a real schema, so they fail if either the bound
// or the partition helper it depends on changes shape.

#include "components/agg_boundary.hpp"
#include "components/history_runs.hpp"
#include "test_env.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

using namespace altspotter::aggregation;
using altspotter::test::TempDb;

namespace {

constexpr int64_t kMinuteMs = 60'000;

// 2026-03-01T00:00:00Z and 2025-12-01T00:00:00Z, the real shape of the incident:
// aggregate partitions began in March, 1m history reached back to December.
constexpr int64_t kMarch = 1772323200000LL;
constexpr int64_t kDecember = 1764547200000LL;

std::string iso(int64_t ms) {
    return "to_timestamp(" + std::to_string(ms) + "::bigint / 1000.0)";
}

// One closed 1m candle. Deliberately valid against chk_ohlc so the insert
// exercises the same constraints production rows do.
void insert_1m(pqxx::connection& c, int ex, long long instr, int64_t open_ms) {
    pqxx::work tx{c};
    tx.exec("INSERT INTO market_candles_1m (exchange_id, instrument_id, open_time, close_time,"
            " open, high, low, close, base_volume, quote_volume, is_closed, source) VALUES (" +
            std::to_string(ex) + "," + std::to_string(instr) + "," + iso(open_ms) + "," +
            iso(open_ms + kMinuteMs) + ",100,100,100,100,1,100,true,'rest_bootstrap')");
    // chk_1m_times: close = open + 1 minute. 'rest_bootstrap' is the source the
    // December 2025 rows of the incident carried.
    tx.commit();
}

void insert_agg(pqxx::connection& c, int ex, long long instr, const std::string& tf,
                int64_t open_ms, int64_t minutes) {
    pqxx::work tx{c};
    tx.exec("INSERT INTO market_candles_agg (exchange_id, instrument_id, timeframe, open_time,"
            " close_time, open, high, low, close, base_volume, quote_volume, is_closed,"
            " quality_status, minute_count) VALUES (" +
            std::to_string(ex) + "," + std::to_string(instr) + ",'" + tf + "'," + iso(open_ms) +
            "," + iso(open_ms + minutes * kMinuteMs - 1) +
            ",100,100,100,100,1,100,true,'ok'," + std::to_string(minutes) + ")");
    tx.commit();
}

// The production catch-up statement. Returns the first window that is both
// complete in 1m and missing from the aggregate, or nullopt.
std::optional<int64_t> first_missing(pqxx::connection& c, int ex, long long instr,
                                     const std::string& tf, int minutes, int64_t bound_ms,
                                     int64_t floor_ms) {
    pqxx::work tx{c};
    tx.exec("SET LOCAL TIME ZONE 'UTC'");
    const auto r = tx.exec_params(kFirstMissingWindowSql, ex, instr,
                                  std::to_string(minutes) + " minutes", tf, bound_ms, minutes,
                                  floor_ms);
    if (r.empty() || r[0][0].is_null()) return std::nullopt;
    return r[0][0].as<int64_t>();
}

int64_t aggregate_floor(pqxx::connection& c) {
    pqxx::work tx{c};
    tx.exec("SET LOCAL TIME ZONE 'UTC'");
    const auto r = tx.exec(kAggregateFloorSql);
    if (r.empty() || r[0][0].is_null()) return kNoAggregatePartition;
    return r[0][0].as<int64_t>();
}

// A full 5m window of 1m candles starting at open_ms.
void insert_full_5m(pqxx::connection& c, int ex, long long instr, int64_t open_ms) {
    for (int i = 0; i < 5; ++i) insert_1m(c, ex, instr, open_ms + i * kMinuteMs);
}

} // namespace

// ---------------------------------------------------------------------------
// Pure boundary logic. No database: these pin the decision itself.
// ---------------------------------------------------------------------------

TEST_CASE("the floor decides which windows may be written", "[aggregation][boundary]") {
    SECTION("a window at or after the floor is writable") {
        REQUIRE(window_is_writable(kMarch, kMarch));
        REQUIRE(window_is_writable(kMarch + kMinuteMs, kMarch));
    }

    SECTION("a window before the floor is not") {
        REQUIRE_FALSE(window_is_writable(kDecember, kMarch));
        REQUIRE_FALSE(window_is_writable(kMarch - 1, kMarch));
    }

    // Scenario 8: partition maintenance has failed outright. Nothing can land,
    // so nothing is eligible -- the opposite of the COALESCE('epoch') fallback,
    // which made *everything* eligible in exactly this situation.
    SECTION("no aggregate partition at all blocks every window") {
        REQUIRE_FALSE(window_is_writable(kDecember, kNoAggregatePartition));
        REQUIRE_FALSE(window_is_writable(kMarch, kNoAggregatePartition));
        REQUIRE_FALSE(window_is_writable(INT64_MAX - 1, kNoAggregatePartition));
    }

    // An unknown floor must never block live publication: that would turn a
    // transient catalog error into a data outage.
    SECTION("an unestablished floor is permissive") {
        REQUIRE(floor_is_known(kMarch));
        REQUIRE_FALSE(floor_is_known(kFloorUnknown));
        REQUIRE(window_is_writable(kDecember, kFloorUnknown));
    }
}

// Scenario 7: the repair path is handed a range derived from a 1m gap.
TEST_CASE("a repair range is clamped to the writable region", "[aggregation][boundary][repair]") {
    SECTION("a range entirely below the floor is dropped") {
        REQUIRE_FALSE(clamp_to_writable(kDecember, kDecember + 3600'000, kMarch).has_value());
    }

    SECTION("a range straddling the floor keeps only its writable tail") {
        const auto r = clamp_to_writable(kDecember, kMarch + 3600'000, kMarch);
        REQUIRE(r.has_value());
        REQUIRE(r->first == kMarch);
        REQUIRE(r->second == kMarch + 3600'000);
    }

    SECTION("a range entirely above the floor is untouched") {
        const auto r = clamp_to_writable(kMarch, kMarch + 3600'000, kMarch);
        REQUIRE(r.has_value());
        REQUIRE(r->first == kMarch);
        REQUIRE(r->second == kMarch + 3600'000);
    }

    SECTION("a range that ends exactly at the floor is dropped") {
        REQUIRE_FALSE(clamp_to_writable(kDecember, kMarch, kMarch).has_value());
    }

    SECTION("no aggregate partition drops every range") {
        REQUIRE_FALSE(
            clamp_to_writable(kMarch, kMarch + 3600'000, kNoAggregatePartition).has_value());
    }
}

// ---------------------------------------------------------------------------
// The real statements against the real schema.
// ---------------------------------------------------------------------------

TEST_CASE("the catch-up scan respects the aggregate partition floor",
          "[aggregation][boundary][db]") {
    if (!TempDb::admin_dsn()) {
        SKIP("PostgreSQL not available (set ALTSPOTTER_TEST_PG_DSN)");
    }
    TempDb db;
    REQUIRE(db.create());
    const std::string bin = altspotter::test::bin_dir() + "/altspotter";
    REQUIRE(altspotter::test::run_tool(
                bin, {"migrate", "--dsn", db.dsn(), "--migrations",
                      altspotter::test::migrations_dir()}) == 0);

    pqxx::connection c{db.dsn()};

    // The incident shape: 1m partitions from December, aggregate from March.
    {
        pqxx::work tx{c};
        // The partition helper turns a month DATE into its bound in the
        // SESSION time zone. Production runs in Etc/UTC (VPS1 verified); a test
        // host in another zone would otherwise shift every bound by its offset.
        tx.exec("SET LOCAL TIME ZONE 'UTC'");
        tx.exec("SELECT altspotter_ensure_candle_partitions(" + iso(kDecember) + "," +
                iso(kMarch + 86400'000LL * 40) + ")");
        // Retention dropped the aggregate partitions before March.
        tx.exec("DROP TABLE IF EXISTS market_candles_agg_202512");
        tx.exec("DROP TABLE IF EXISTS market_candles_agg_202601");
        tx.exec("DROP TABLE IF EXISTS market_candles_agg_202602");
        tx.commit();
    }

    // Scenario 1: the floor is the lower bound of the oldest surviving
    // aggregate partition -- March, not December, and not 'epoch'.
    const int64_t floor_ms = aggregate_floor(c);
    REQUIRE(floor_ms == kMarch);

    constexpr int kEx = 4;
    constexpr long long kInstr = 77;
    const int64_t bound = kMarch + 86400'000LL * 30;

    SECTION("historical 1m below the floor is never eligible") {
        // Scenario 2 and scenario 5 together: a complete 1m window in December,
        // and the key has no aggregate rows at all -- the exact state in which
        // COALESCE(min(open_time),'epoch') collapsed and re-opened the history.
        insert_full_5m(c, kEx, kInstr, kDecember);
        REQUIRE_FALSE(first_missing(c, kEx, kInstr, "5m", 5, bound, floor_ms).has_value());

        // Scenario 9: the answer is stable. Nothing advances, nothing is
        // retried, so there is no scan storm -- the loop simply has no fuel.
        for (int i = 0; i < 5; ++i) {
            REQUIRE_FALSE(first_missing(c, kEx, kInstr, "5m", 5, bound, floor_ms).has_value());
        }
    }

    SECTION("an in-retention gap above the floor is still found") {
        // Scenario 3: the fix must not blind normal catch-up.
        insert_full_5m(c, kEx, kInstr + 1, kMarch);
        const auto w = first_missing(c, kEx, kInstr + 1, "5m", 5, bound, floor_ms);
        REQUIRE(w.has_value());
        REQUIRE(*w == kMarch);
    }

    SECTION("mixed historical and in-range data yields only the in-range window") {
        // Scenario 4.
        insert_full_5m(c, kEx, kInstr + 2, kDecember);           // unwritable
        insert_full_5m(c, kEx, kInstr + 2, kMarch + 3600'000);   // writable
        const auto w = first_missing(c, kEx, kInstr + 2, "5m", 5, bound, floor_ms);
        REQUIRE(w.has_value());
        REQUIRE(*w == kMarch + 3600'000);   // NOT the December window
    }

    SECTION("a key that already has aggregates is bounded the same way") {
        // Scenario 1 again from the other side: an existing aggregate row must
        // not widen the scan below the partition floor.
        insert_full_5m(c, kEx, kInstr + 3, kDecember);
        insert_full_5m(c, kEx, kInstr + 3, kMarch);
        insert_agg(c, kEx, kInstr + 3, "5m", kMarch, 5);
        REQUIRE_FALSE(first_missing(c, kEx, kInstr + 3, "5m", 5, bound, floor_ms).has_value());
    }

    SECTION("a newly configured timeframe bootstraps from the floor, not from epoch") {
        // Scenario 6: '1h' has no aggregate row anywhere in the database. The
        // old bound collapsed to 'epoch' here and made December eligible; the
        // partition floor keeps the bootstrap inside the writable region.
        insert_full_5m(c, kEx, kInstr + 4, kDecember);
        for (int i = 0; i < 60; ++i) insert_1m(c, kEx, kInstr + 4, kMarch + i * kMinuteMs);

        const auto w = first_missing(c, kEx, kInstr + 4, "1h", 60, bound, floor_ms);
        REQUIRE(w.has_value());
        REQUIRE(*w == kMarch);          // bootstrap works
        REQUIRE(*w >= floor_ms);        // and stays writable
    }

    SECTION("every window the scan returns is writable") {
        // The property the writer depends on, stated directly: whatever the
        // scan hands back, publish_windows would accept.
        insert_full_5m(c, kEx, kInstr + 5, kDecember);
        insert_full_5m(c, kEx, kInstr + 5, kMarch + 7200'000);
        if (const auto w = first_missing(c, kEx, kInstr + 5, "5m", 5, bound, floor_ms)) {
            REQUIRE(window_is_writable(*w, floor_ms));
        }
    }
}

// Scenario 8: partition maintenance has failed outright and market_candles_agg
// has no range partition at all. `altspotter migrate` pre-creates partitions
// around the current date, so the state is reached by dropping every one of
// them -- what a maintenance outage plus retention would eventually leave.
TEST_CASE("with no aggregate partition the floor blocks every window",
          "[aggregation][boundary][db]") {
    if (!TempDb::admin_dsn()) {
        SKIP("PostgreSQL not available (set ALTSPOTTER_TEST_PG_DSN)");
    }
    TempDb db;
    REQUIRE(db.create());
    const std::string bin = altspotter::test::bin_dir() + "/altspotter";
    REQUIRE(altspotter::test::run_tool(
                bin, {"migrate", "--dsn", db.dsn(), "--migrations",
                      altspotter::test::migrations_dir()}) == 0);
    pqxx::connection c{db.dsn()};

    REQUIRE(aggregate_floor(c) != kNoAggregatePartition);   // migrate made some
    {
        pqxx::work tx{c};
        const auto parts = tx.exec(
            "SELECT partition_name FROM altspotter_partition_bounds('market_candles_agg'::regclass)");
        REQUIRE_FALSE(parts.empty());
        for (const auto& r : parts) tx.exec("DROP TABLE " + tx.quote_name(r[0].as<std::string>()));
        tx.commit();
    }

    // The old bound's COALESCE fallback made EVERY window eligible here; the
    // partition floor makes none of them eligible, which is the only answer a
    // table that can accept no row can give.
    const int64_t floor_ms = aggregate_floor(c);
    REQUIRE(floor_ms == kNoAggregatePartition);
    REQUIRE_FALSE(window_is_writable(kDecember, floor_ms));
    REQUIRE_FALSE(window_is_writable(kMarch, floor_ms));
    REQUIRE_FALSE(clamp_to_writable(kMarch, kMarch + 3600'000, floor_ms).has_value());
}

// ===========================================================================
// Periodic catch-up: how far back a pass looks.
//
// On VPS1 every periodic pass re-scanned the whole retained 1m history from the
// partition floor: ~5 s per instrument, ~540 s per pass, during which the one
// aggregation loop published nothing. 15m bars closing early in a pass reached
// AltStrategy past its 20-minute max_component_age. A periodic pass now looks
// back only as far as the writer can still have changed anything.
// ===========================================================================

// Pure plan logic: which ranges one key's periodic pass scans.
TEST_CASE("a catch-up plan scans the recent range and each backfill's writable part",
          "[aggregation][catchup]") {
    constexpr int64_t kHorizon = 35 * kMinuteMs;
    constexpr int64_t kDay = 86'400'000;
    const int64_t floor = kMarch;
    const int64_t frontier = kMarch + 30 * kDay;
    const ScanRange recent{frontier - kHorizon, kOpenEnded};
    // A repair as market_history records it: the inclusive last minute, at most
    // max_gap_minutes_per_command (1000) minutes long.
    const auto repair = [](int64_t from, int64_t minutes) {
        return BackfillRun{from, from + (minutes - 1) * kMinuteMs};
    };
    using Ranges = std::vector<ScanRange>;

    SECTION("a key never passed is scanned from the floor (startup, new key)") {
        const auto plan = plan_catchup(floor, std::nullopt, kHorizon, {repair(kDecember, 60)});
        REQUIRE(plan.ranges == Ranges{{floor, kOpenEnded}});
        REQUIRE(plan.full_reach(floor));
    }
    SECTION("test 7: without a backfill, only the recent range") {
        const auto plan = plan_catchup(floor, frontier, kHorizon, {});
        REQUIRE(plan.ranges == Ranges{recent});
        REQUIRE_FALSE(plan.full_reach(floor));
    }
    SECTION("test 1: a repair wholly below the floor adds nothing") {
        const auto plan = plan_catchup(floor, frontier, kHorizon, {repair(kDecember, 1000)});
        REQUIRE(plan.ranges == Ranges{recent});
        REQUIRE(plan.runs_below_floor == 1);
        // ...including one whose last minute is the minute before the floor.
        const auto edge = plan_catchup(floor, frontier, kHorizon,
                                       {repair(floor - 1000 * kMinuteMs, 1000)});
        REQUIRE(edge.ranges == Ranges{recent});
        REQUIRE(edge.runs_below_floor == 1);
    }
    SECTION("test 2: a repair straddling the floor is cut at it") {
        const auto plan =
            plan_catchup(floor, frontier, kHorizon, {repair(floor - 30 * kMinuteMs, 60)});
        REQUIRE(plan.ranges == Ranges{{floor, floor + 30 * kMinuteMs}, recent});
        REQUIRE(plan.runs_clamped == 1);
        REQUIRE_FALSE(plan.full_reach(floor));   // cut at the floor is not a deep scan
    }
    SECTION("test 3: a repair wholly above the floor is scanned as recorded") {
        const int64_t from = kMarch + 5 * kDay;
        const auto plan = plan_catchup(floor, frontier, kHorizon, {repair(from, 1000)});
        REQUIRE(plan.ranges == Ranges{{from, from + 1000 * kMinuteMs}, recent});
        REQUIRE(plan.runs_within == 1);
    }
    SECTION("test 8: a repair inside the recent range costs nothing extra") {
        const auto plan =
            plan_catchup(floor, frontier, kHorizon, {repair(frontier - 10 * kMinuteMs, 5)});
        REQUIRE(plan.ranges == Ranges{recent});
    }
    SECTION("test 9: many repairs: overlapping ones coalesce, the rest stay apart") {
        const int64_t a = kMarch + 2 * kDay;
        const int64_t b = kMarch + 9 * kDay;
        const auto plan = plan_catchup(floor, frontier, kHorizon,
                                       {repair(b, 1000), repair(a, 1000),
                                        repair(a + 500 * kMinuteMs, 1000),
                                        repair(a + 1500 * kMinuteMs, 10)});
        REQUIRE(plan.ranges ==
                Ranges{{a, a + 1510 * kMinuteMs}, {b, b + 1000 * kMinuteMs}, recent});
        REQUIRE(plan.runs_within == 4);
    }
    SECTION("test 10: when every repair is below the floor, the plan is the no-repair plan") {
        std::vector<BackfillRun> runs;
        for (int i = 0; i < 50; ++i) runs.push_back(repair(kDecember + i * kDay, 1000));
        const auto plan = plan_catchup(floor, frontier, kHorizon, runs);
        REQUIRE(plan.ranges == plan_catchup(floor, frontier, kHorizon, {}).ranges);
        REQUIRE(plan.runs_below_floor == 50);
    }
    SECTION("test 11: a mix keeps only the writable repairs") {
        const int64_t above = kMarch + 12 * kDay;
        const auto plan = plan_catchup(floor, frontier, kHorizon,
                                       {repair(kDecember, 1000), repair(above, 100),
                                        repair(kDecember + 40 * kDay, 1000)});
        REQUIRE(plan.ranges == Ranges{{above, above + 100 * kMinuteMs}, recent});
        REQUIRE(plan.runs_below_floor == 2);
        REQUIRE(plan.runs_within == 1);
    }
    SECTION("test 12: a repair months back is scanned over its own minutes, not up to now") {
        const int64_t from = kMarch + kDay;   // 29 days before the frontier
        const auto plan = plan_catchup(floor, frontier, kHorizon, {repair(from, 1000)});
        REQUIRE(plan_span_ms(plan.ranges, frontier) == 1000 * kMinuteMs + kHorizon);
    }
    SECTION("test 13: a writer that fell hours behind pulls the recent range back with it") {
        const int64_t lagging = frontier - 3 * 3'600'000LL;
        const auto plan = plan_catchup(floor, lagging, kHorizon, {});
        REQUIRE(plan.ranges == Ranges{{lagging - kHorizon, kOpenEnded}});
    }
    SECTION("the recent range never starts below the floor") {
        const auto plan = plan_catchup(floor, floor + kMinuteMs, kHorizon, {});
        REQUIRE(plan.ranges == Ranges{{floor, kOpenEnded}});
    }
    SECTION("a bootstrap reaching below the floor is the one deep periodic scan") {
        // market_history records a bootstrap's start time as requested_to; one
        // started around the previous pass runs into the recent range.
        const auto plan =
            plan_catchup(floor, frontier, kHorizon, {BackfillRun{kDecember, frontier}});
        REQUIRE(plan.full_reach(floor));
        REQUIRE(plan.runs_clamped == 1);
        // A newly listed instrument's bootstrap is long, but starts at its listing.
        const int64_t listed = kMarch + 20 * kDay;
        const auto listing = plan_catchup(floor, frontier, kHorizon, {BackfillRun{listed, frontier}});
        REQUIRE(listing.ranges == Ranges{{listed, kOpenEnded}});
        REQUIRE_FALSE(listing.full_reach(floor));
    }
    SECTION("an unknown end is read as reaching as far as it could") {
        const auto no_from =
            plan_catchup(floor, frontier, kHorizon, {BackfillRun{std::nullopt, kMarch + kDay}});
        REQUIRE(no_from.ranges.front() == ScanRange{floor, kMarch + kDay + kMinuteMs});
        const auto no_to =
            plan_catchup(floor, frontier, kHorizon, {BackfillRun{kMarch + kDay, std::nullopt}});
        REQUIRE(no_to.ranges == Ranges{{kMarch + kDay, kOpenEnded}});
    }
    SECTION("no plan holds a range below the floor, and no two ranges touch") {
        uint64_t x = 88172645463325252ULL;   // xorshift: deterministic, seed fixed
        const auto next = [&] {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            return x;
        };
        for (int trial = 0; trial < 2000; ++trial) {
            std::vector<BackfillRun> runs;
            const int n = static_cast<int>(next() % 8);
            for (int i = 0; i < n; ++i) {
                const int64_t from =
                    kDecember + static_cast<int64_t>(next() % (130ULL * kDay / kMinuteMs)) * kMinuteMs;
                runs.push_back(repair(from, 1 + static_cast<int64_t>(next() % 1000)));
            }
            const auto plan = plan_catchup(floor, frontier, kHorizon, runs);
            bool sound = plan.runs_below_floor + plan.runs_clamped + plan.runs_within == n;
            for (size_t i = 0; i < plan.ranges.size(); ++i) {
                sound = sound && plan.ranges[i].from_ms >= floor &&
                        plan.ranges[i].from_ms < plan.ranges[i].to_ms &&
                        (i == 0 || plan.ranges[i - 1].to_ms < plan.ranges[i].from_ms);
            }
            CAPTURE(trial, n);
            REQUIRE(sound);
        }
    }
}

TEST_CASE("a scan bound is aligned to the timeframe grid", "[aggregation][catchup]") {
    const int64_t h1 = 3'600'000;
    // 10:37 inside the 10:00 hour: the 10:00 window must still be scanned whole.
    const int64_t t = kMarch + 10 * h1 + 37 * kMinuteMs;
    REQUIRE(scan_from_for_timeframe(t, kMarch, 5 * kMinuteMs) == kMarch + 10 * h1 + 35 * kMinuteMs);
    REQUIRE(scan_from_for_timeframe(t, kMarch, h1) == kMarch + 10 * h1);
    REQUIRE(scan_from_for_timeframe(t, kMarch, 24 * h1) == kMarch);
    REQUIRE(scan_from_for_timeframe(kMarch + kMinuteMs, kMarch + kMinuteMs, h1) == kMarch + kMinuteMs);
}

TEST_CASE("a plan is widened to whole windows of each timeframe", "[aggregation][catchup]") {
    using Ranges = std::vector<ScanRange>;
    const int64_t h1 = 3'600'000;
    const int64_t d1 = 24 * h1;
    const int64_t day = kMarch + 10 * d1;
    const int64_t closed = kMarch + 30 * d1;
    // A repair of 10:37 .. 11:12 inclusive, i.e. [10:37, 11:13).
    const Ranges repair{{day + 10 * h1 + 37 * kMinuteMs, day + 11 * h1 + 13 * kMinuteMs}};

    SECTION("start down, end up, on every grid") {
        REQUIRE(ranges_for_timeframe(repair, kMarch, 5 * kMinuteMs, closed) ==
                Ranges{{day + 10 * h1 + 35 * kMinuteMs, day + 11 * h1 + 15 * kMinuteMs}});
        REQUIRE(ranges_for_timeframe(repair, kMarch, h1, closed) ==
                Ranges{{day + 10 * h1, day + 12 * h1}});
        REQUIRE(ranges_for_timeframe(repair, kMarch, d1, closed) == Ranges{{day, day + d1}});
    }
    SECTION("an aligned end is not widened") {
        REQUIRE(ranges_for_timeframe({{day, day + h1}}, kMarch, h1, closed) ==
                Ranges{{day, day + h1}});
    }
    SECTION("an open range runs to the closed bound") {
        REQUIRE(ranges_for_timeframe({{day, kOpenEnded}}, kMarch, 5 * kMinuteMs, closed) ==
                Ranges{{day, closed}});
    }
    SECTION("widening coalesces what it makes overlap") {
        const Ranges two{{day + 2 * h1, day + 2 * h1 + 10 * kMinuteMs},
                         {day + 20 * h1, day + 20 * h1 + 10 * kMinuteMs}};
        REQUIRE(ranges_for_timeframe(two, kMarch, 5 * kMinuteMs, closed).size() == 2);
        REQUIRE(ranges_for_timeframe(two, kMarch, d1, closed) == Ranges{{day, day + d1}});
    }
    SECTION("nothing at or past the closed bound") {
        REQUIRE(ranges_for_timeframe({{closed, closed + h1}}, kMarch, h1, closed).empty());
        REQUIRE(ranges_for_timeframe({{closed - h1, closed + h1}}, kMarch, h1, closed) ==
                Ranges{{closed - h1, closed}});
    }
    SECTION("never below the floor") {
        REQUIRE(ranges_for_timeframe({{kMarch + 7 * kMinuteMs, kMarch + h1}}, kMarch, d1, closed) ==
                Ranges{{kMarch, kMarch + d1}});
    }
}

namespace {

// Rows actually read from market_candles_1m by one execution of the catch-up
// statement, summed over the partitions the plan touched. Deterministic, unlike
// a stopwatch: this is the work that made a pass take minutes.
int64_t rows_read_from_1m(pqxx::connection& c, int ex, long long instr, const std::string& tf,
                          int minutes, int64_t bound_ms, int64_t scan_from_ms) {
    pqxx::work tx{c};
    tx.exec("SET LOCAL TIME ZONE 'UTC'");
    const auto r = tx.exec_params(std::string("EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ") +
                                      kFirstMissingWindowSql,
                                  ex, instr, std::to_string(minutes) + " minutes", tf, bound_ms,
                                  minutes, scan_from_ms);
    const std::regex scan(R"(on market_candles_1m_\w+.*actual rows=(\d+) loops=(\d+))");
    int64_t total = 0;
    for (const auto& row : r) {
        const std::string line = row[0].as<std::string>();
        // A bitmap scan reports its rows twice, on the index node and on the
        // heap node; the heap node is the one that read them.
        if (line.find("Bitmap Index Scan") != std::string::npos) continue;
        std::smatch m;
        if (std::regex_search(line, m, scan)) total += std::stoll(m[1]) * std::stoll(m[2]);
    }
    return total;
}

// What one pass of `ranges` reads on one timeframe, and what it finds: the
// same per-range executions catch_up issues.
int64_t rows_read_by_plan(pqxx::connection& c, int ex, long long instr, const std::string& tf,
                          int minutes, const std::vector<ScanRange>& ranges, int64_t floor_ms,
                          int64_t closed_ms) {
    int64_t total = 0;
    for (const auto& r : ranges_for_timeframe(ranges, floor_ms, minutes * kMinuteMs, closed_ms)) {
        total += rows_read_from_1m(c, ex, instr, tf, minutes, r.to_ms, r.from_ms);
    }
    return total;
}

std::vector<int64_t> found_by_plan(pqxx::connection& c, int ex, long long instr,
                                   const std::string& tf, int minutes,
                                   const std::vector<ScanRange>& ranges, int64_t floor_ms,
                                   int64_t closed_ms) {
    std::vector<int64_t> found;
    for (const auto& r : ranges_for_timeframe(ranges, floor_ms, minutes * kMinuteMs, closed_ms)) {
        if (const auto w = first_missing(c, ex, instr, tf, minutes, r.to_ms, r.from_ms)) {
            found.push_back(*w);
        }
    }
    return found;
}

std::optional<int64_t> frontier_of(pqxx::connection& c, int ex, long long instr) {
    pqxx::work tx{c};
    const auto r = tx.exec_params(kWriterFrontierSql, ex, instr);
    if (r.empty() || r[0][0].is_null()) return std::nullopt;
    return r[0][0].as<int64_t>();
}

std::vector<BackfillRun> backfill_runs(pqxx::connection& c, int ex, long long instr,
                                       int64_t committed_through_ms, bool owner_alive) {
    pqxx::work tx{c};
    std::vector<BackfillRun> runs;
    for (const auto& r :
         tx.exec_params(kBackfillRunsSql, ex, instr, committed_through_ms, owner_alive)) {
        BackfillRun run;
        if (!r[0].is_null()) run.from_ms = r[0].as<int64_t>();
        if (!r[1].is_null()) run.to_ms = r[1].as<int64_t>();
        runs.push_back(run);
    }
    return runs;
}

bool run_owner_alive(pqxx::connection& c) {
    pqxx::work tx{c};
    return tx.exec_params(altspotter::history::kRunOwnerAliveSql,
                          altspotter::history::kRunOwnerLockSpace,
                          altspotter::history::kRunOwnerLockKey)[0][0]
        .as<bool>();
}

void drop_agg(pqxx::connection& c, int ex, long long instr, int64_t open_ms) {
    pqxx::work tx{c};
    tx.exec("DELETE FROM market_candles_agg WHERE exchange_id = " + std::to_string(ex) +
            " AND instrument_id = " + std::to_string(instr) + " AND timeframe = '5m' AND open_time = " +
            iso(open_ms));
    tx.commit();
}

// A REST repair as market_history records it: [from, from + minutes) with the
// inclusive last minute in requested_to.
void record_repair(pqxx::connection& c, int ex, long long instr, int64_t from_ms, int64_t minutes,
                   const std::string& status, int64_t finished_ms) {
    pqxx::work tx{c};
    tx.exec("INSERT INTO ingestion_runs (service, kind, exchange_id, instrument_id,"
            " requested_from, requested_to, status, started_at, finished_at) VALUES"
            " ('market-history-service', 'repair', " + std::to_string(ex) + ", " +
            std::to_string(instr) + ", " + iso(from_ms) + ", " +
            iso(from_ms + (minutes - 1) * kMinuteMs) + ", '" + status + "', " +
            iso(finished_ms - kMinuteMs) + ", " +
            (status == "running" ? std::string("NULL") : iso(finished_ms)) + ")");
    tx.commit();
}

} // namespace

TEST_CASE("a periodic pass reads its plan, not the history", "[aggregation][catchup][db]") {
    if (!TempDb::admin_dsn()) {
        SKIP("PostgreSQL not available (set ALTSPOTTER_TEST_PG_DSN)");
    }
    TempDb db;
    REQUIRE(db.create());
    const std::string bin = altspotter::test::bin_dir() + "/altspotter";
    REQUIRE(altspotter::test::run_tool(bin, {"migrate", "--dsn", db.dsn(), "--migrations",
                                              altspotter::test::migrations_dir()}) == 0);
    pqxx::connection c{db.dsn()};

    constexpr int kEx = 1;
    constexpr long long kKey = 500;
    constexpr int64_t kDay = 86'400'000;
    constexpr int64_t kHorizon = 35 * kMinuteMs;
    const int64_t end = kMarch + 30 * kDay;   // 30 writable days, plus the band below
    {
        pqxx::work tx{c};
        tx.exec("SET LOCAL TIME ZONE 'UTC'");
        // The incident shape again: 1m from December, aggregates from March.
        tx.exec("SELECT altspotter_ensure_candle_partitions(" + iso(kDecember) + "," +
                iso(end + kDay) + ")");
        tx.exec("DROP TABLE IF EXISTS market_candles_agg_202512");
        tx.exec("DROP TABLE IF EXISTS market_candles_agg_202601");
        tx.exec("DROP TABLE IF EXISTS market_candles_agg_202602");
        tx.exec("INSERT INTO market_candles_1m (exchange_id, instrument_id, open_time, close_time,"
                " open, high, low, close, base_volume, quote_volume, is_closed, source)"
                " SELECT " + std::to_string(kEx) + "," + std::to_string(kKey) +
                ", t, t + interval '1 minute', 100,100,100,100,1,100, true, 'ws'"
                " FROM generate_series(" + iso(kDecember) + ", " + iso(end - kMinuteMs) +
                ", interval '1 minute') t");
        // Every writable 5m window already aggregated: the steady state a
        // periodic pass meets on VPS1.
        tx.exec("INSERT INTO market_candles_agg (exchange_id, instrument_id, timeframe, open_time,"
                " close_time, open, high, low, close, base_volume, quote_volume, is_closed,"
                " quality_status, minute_count)"
                " SELECT exchange_id, instrument_id, '5m', w, w + interval '5 minutes' - interval '1 millisecond',"
                " 100,100,100,100, count(*), 100*count(*), true, 'ok', count(*)"
                " FROM (SELECT exchange_id, instrument_id,"
                "       date_bin('5 minutes', open_time, 'epoch'::timestamptz) w"
                "       FROM market_candles_1m WHERE exchange_id = " + std::to_string(kEx) +
                "       AND instrument_id = " + std::to_string(kKey) + " AND open_time >= " +
                iso(kMarch) + ") s"
                " GROUP BY exchange_id, instrument_id, w");
        tx.commit();
    }
    {
        pqxx::nontransaction tx{c};
        tx.exec("ANALYZE market_candles_1m");
        tx.exec("ANALYZE market_candles_agg");
    }

    const int64_t floor = aggregate_floor(c);
    REQUIRE(floor == kMarch);
    const auto frontier = frontier_of(c, kEx, kKey);
    REQUIRE(frontier == end - kMinuteMs);   // the writer frontier: the newest committed minute
    // The previous pass saw this frontier. A repair that finished after it,
    // less the horizon, may have landed since, so it is looked at; the pass
    // runs "now", at `end`.
    const int64_t committed_through = *frontier - kHorizon;
    const auto plan_now = [&] {
        return plan_catchup(floor, frontier, kHorizon,
                            backfill_runs(c, kEx, kKey, committed_through, false));
    };
    const auto rows_5m = [&](const CatchUpPlan& p) {
        return rows_read_by_plan(c, kEx, kKey, "5m", 5, p.ranges, floor, end);
    };
    const auto found_5m = [&](const CatchUpPlan& p) {
        return found_by_plan(c, kEx, kKey, "5m", 5, p.ranges, floor, end);
    };
    const int64_t full_rows = rows_read_from_1m(c, kEx, kKey, "5m", 5, end, floor);
    REQUIRE(full_rows == 30 * 1440);   // what every periodic pass read before any of this
    const int64_t recent_rows = rows_5m(plan_catchup(floor, frontier, kHorizon, {}));
    REQUIRE(recent_rows <= 40);        // 35-minute horizon, aligned to the 5m grid

    SECTION("test 7: without a repair a pass reads the recent range only") {
        const auto plan = plan_now();
        REQUIRE(plan.ranges.size() == 1);
        REQUIRE(rows_5m(plan) == recent_rows);
        drop_agg(c, kEx, kKey, kMarch + 5 * kDay);   // an old gap stays for startup
        REQUIRE(found_5m(plan).empty());
    }

    SECTION("test 8: one recent repair is rescanned over its own range") {
        const int64_t from = kMarch + 5 * kDay;
        record_repair(c, kEx, kKey, from, 1000, "completed", end);
        drop_agg(c, kEx, kKey, from + 100 * kMinuteMs);   // inside the repair
        drop_agg(c, kEx, kKey, kMarch + 10 * kDay);        // outside it
        const auto plan = plan_now();
        CAPTURE(rows_5m(plan));
        REQUIRE(rows_5m(plan) <= recent_rows + 1000 + 5);
        REQUIRE(found_5m(plan) == std::vector<int64_t>{from + 100 * kMinuteMs});
    }

    SECTION("test 9: many recent repairs cost the sum of their own ranges") {
        std::vector<int64_t> gaps;
        for (int i = 0; i < 12; ++i) {
            const int64_t from = kMarch + (1 + 2 * i) * kDay;
            record_repair(c, kEx, kKey, from, 1000, "completed", end);
            gaps.push_back(from + 500 * kMinuteMs);
            drop_agg(c, kEx, kKey, gaps.back());
        }
        const auto plan = plan_now();
        CAPTURE(rows_5m(plan));
        REQUIRE(plan.runs_within == 12);
        REQUIRE(rows_5m(plan) <= recent_rows + 12 * (1000 + 5));
        REQUIRE(found_5m(plan) == gaps);
    }

    SECTION("test 10: repairs wholly below the floor cost nothing") {
        for (int i = 0; i < 20; ++i) {
            record_repair(c, kEx, kKey, kDecember + i * 4 * kDay, 1000, "completed", end);
        }
        const auto plan = plan_now();
        REQUIRE(plan.runs_below_floor == 20);
        REQUIRE(plan.ranges == plan_catchup(floor, frontier, kHorizon, {}).ranges);
        REQUIRE(rows_5m(plan) == recent_rows);
        REQUIRE(found_5m(plan).empty());   // below the floor nothing is ever eligible
    }

    SECTION("test 11: a mix reads only the writable repairs") {
        for (int i = 0; i < 10; ++i) {
            record_repair(c, kEx, kKey, kDecember + i * 8 * kDay, 1000, "completed", end);
        }
        record_repair(c, kEx, kKey, kMarch + 3 * kDay, 1000, "completed", end);
        record_repair(c, kEx, kKey, kMarch + 20 * kDay, 1000, "completed", end);
        const auto plan = plan_now();
        REQUIRE(plan.runs_below_floor == 10);
        REQUIRE(plan.runs_within == 2);
        REQUIRE(rows_5m(plan) <= recent_rows + 2 * (1000 + 5));
    }

    SECTION("test 12: a repair a month back does not pull the scan up to now") {
        const int64_t from = kMarch + kDay;
        record_repair(c, kEx, kKey, from, 1000, "completed", end);
        drop_agg(c, kEx, kKey, from + 200 * kMinuteMs);   // inside: found
        drop_agg(c, kEx, kKey, kMarch + 10 * kDay);        // between it and now: not
        const auto plan = plan_now();
        const int64_t rows = rows_5m(plan);
        // The single lower bound it replaces read from the repair to now.
        const int64_t old_rows = rows_read_from_1m(c, kEx, kKey, "5m", 5, end, from);
        CAPTURE(rows, old_rows);
        REQUIRE(rows <= recent_rows + 1000 + 5);
        REQUIRE(old_rows >= 28 * 1440);
        REQUIRE(found_5m(plan) == std::vector<int64_t>{from + 200 * kMinuteMs});
    }

    SECTION("test 2: a repair straddling the floor is rescanned from the floor only") {
        record_repair(c, kEx, kKey, floor - 30 * kMinuteMs, 60, "completed", end);
        drop_agg(c, kEx, kKey, floor);
        const auto plan = plan_now();
        REQUIRE(plan.runs_clamped == 1);
        REQUIRE(rows_5m(plan) <= recent_rows + 30);
        REQUIRE(found_5m(plan) == std::vector<int64_t>{floor});
    }

    SECTION("test 13: a writer that was hours behind at the last pass is caught up in full") {
        const int64_t lagging = end - 3 * 3'600'000LL;
        drop_agg(c, kEx, kKey, end - 2 * 3'600'000LL);
        const auto plan = plan_catchup(floor, lagging, kHorizon, {});
        REQUIRE(found_5m(plan) == std::vector<int64_t>{end - 2 * 3'600'000LL});
    }

    SECTION("a repair committed before the previous pass is not rescanned") {
        record_repair(c, kEx, kKey, kMarch + kDay, 1000, "completed", end - 2 * kDay);
        const auto plan = plan_now();
        REQUIRE(plan.ranges.size() == 1);
        REQUIRE(rows_5m(plan) == recent_rows);
    }

    SECTION("test 16 (reader): 'running' counts only while its owner is alive") {
        const int64_t from = kMarch + 15 * kDay;
        record_repair(c, kEx, kKey, from, 1000, "running", end);
        // No market_history incarnation holds the lock: the row is an orphan.
        REQUIRE_FALSE(run_owner_alive(c));
        REQUIRE(backfill_runs(c, kEx, kKey, committed_through, run_owner_alive(c)).empty());
        {
            // A live incarnation: the lock is held on a session of its own.
            pqxx::connection owner{db.dsn()};
            pqxx::nontransaction tx{owner};
            REQUIRE(tx.exec_params(altspotter::history::kTakeRunOwnershipSql,
                                   altspotter::history::kRunOwnerLockSpace,
                                   altspotter::history::kRunOwnerLockKey)[0][0]
                        .as<bool>());
            REQUIRE(run_owner_alive(c));
            const auto runs = backfill_runs(c, kEx, kKey, committed_through, run_owner_alive(c));
            REQUIRE(runs.size() == 1);
            REQUIRE(runs.front().from_ms == from);
        }
        // The session ended; PostgreSQL released the lock with it.
        bool released = false;
        for (int i = 0; i < 100 && !released; ++i) {
            released = !run_owner_alive(c);
            if (!released) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        REQUIRE(released);
        REQUIRE(backfill_runs(c, kEx, kKey, committed_through, false).empty());
    }
}

// Performance at VPS1's real depth: ~298 days of 1m (December 2025 onwards) and
// ~207 days of aggregates in all six timeframes (March onwards), one key -- the
// depth every VPS1 key had on 2026-09-24. Keys are scanned independently, so
// a pass is the sum of its keys.
TEST_CASE("at VPS1's depth a periodic pass reads exactly its plan", "[aggregation][catchup][db][perf]") {
    if (!TempDb::admin_dsn()) {
        SKIP("PostgreSQL not available (set ALTSPOTTER_TEST_PG_DSN)");
    }
    TempDb db;
    REQUIRE(db.create());
    const std::string bin = altspotter::test::bin_dir() + "/altspotter";
    REQUIRE(altspotter::test::run_tool(bin, {"migrate", "--dsn", db.dsn(), "--migrations",
                                              altspotter::test::migrations_dir()}) == 0);
    pqxx::connection c{db.dsn()};

    constexpr int kEx = 1;
    constexpr long long kKey = 700;
    constexpr int64_t kDay = 86'400'000;
    constexpr int64_t kHorizon = 35 * kMinuteMs;
    const int64_t end = kMarch + 207 * kDay;
    const std::vector<std::pair<std::string, int>> tfs = {
        {"5m", 5}, {"15m", 15}, {"30m", 30}, {"1h", 60}, {"4h", 240}, {"1d", 1440}};
    {
        pqxx::work tx{c};
        tx.exec("SET LOCAL TIME ZONE 'UTC'");
        tx.exec("SELECT altspotter_ensure_candle_partitions(" + iso(kDecember) + "," +
                iso(end + kDay) + ")");
        tx.exec("DROP TABLE IF EXISTS market_candles_agg_202512");
        tx.exec("DROP TABLE IF EXISTS market_candles_agg_202601");
        tx.exec("DROP TABLE IF EXISTS market_candles_agg_202602");
        tx.exec("INSERT INTO market_candles_1m (exchange_id, instrument_id, open_time, close_time,"
                " open, high, low, close, base_volume, quote_volume, is_closed, source)"
                " SELECT " + std::to_string(kEx) + "," + std::to_string(kKey) +
                ", t, t + interval '1 minute', 100,100,100,100,1,100, true, 'ws'"
                " FROM generate_series(" + iso(kDecember) + ", " + iso(end - kMinuteMs) +
                ", interval '1 minute') t");
        for (const auto& [tf, minutes] : tfs) {
            tx.exec("INSERT INTO market_candles_agg (exchange_id, instrument_id, timeframe, open_time,"
                    " close_time, open, high, low, close, base_volume, quote_volume, is_closed,"
                    " quality_status, minute_count)"
                    " SELECT " + std::to_string(kEx) + ", " + std::to_string(kKey) + ", '" + tf +
                    "', w, w + interval '" + std::to_string(minutes) +
                    " minutes' - interval '1 millisecond', 100,100,100,100, count(*), 100*count(*),"
                    " true, 'ok', count(*)"
                    " FROM (SELECT date_bin('" + std::to_string(minutes) +
                    " minutes', open_time, 'epoch'::timestamptz) w FROM market_candles_1m"
                    " WHERE exchange_id = " + std::to_string(kEx) + " AND instrument_id = " +
                    std::to_string(kKey) + " AND open_time >= " + iso(kMarch) + ") s"
                    " GROUP BY w HAVING count(*) = " + std::to_string(minutes));
        }
        tx.commit();
    }
    {
        pqxx::nontransaction tx{c};
        tx.exec("ANALYZE market_candles_1m");
        tx.exec("ANALYZE market_candles_agg");
    }
    const int64_t floor = aggregate_floor(c);
    REQUIRE(floor == kMarch);
    const auto frontier = frontier_of(c, kEx, kKey);
    REQUIRE(frontier == end - kMinuteMs);

    // One pass of `ranges`, all six timeframes: rows read (EXPLAIN ANALYZE,
    // deterministic), rows the plan says it reads, and wall time of the
    // statements catch_up issues.
    struct Cost { int64_t rows = 0, planned = 0, ms = 0; };
    const auto cost = [&](const std::vector<ScanRange>& ranges) {
        Cost out;
        for (const auto& [tf, minutes] : tfs) {
            const int64_t w = minutes * kMinuteMs;
            const int64_t closed = (end / w) * w;
            for (const auto& r : ranges_for_timeframe(ranges, floor, w, closed)) {
                out.planned += (r.to_ms - r.from_ms) / kMinuteMs;
                out.rows += rows_read_from_1m(c, kEx, kKey, tf, minutes, r.to_ms, r.from_ms);
            }
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto& [tf, minutes] : tfs) {
            const int64_t w = minutes * kMinuteMs;
            const int64_t closed = (end / w) * w;
            for (const auto& r : ranges_for_timeframe(ranges, floor, w, closed)) {
                (void)first_missing(c, kEx, kKey, tf, minutes, r.to_ms, r.from_ms);
            }
        }
        out.ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0)
                     .count();
        return out;
    };
    const auto with_repairs = [&](const std::vector<int64_t>& starts) {
        {
            pqxx::work tx{c};
            tx.exec("DELETE FROM ingestion_runs");
            tx.commit();
        }
        for (const int64_t from : starts) record_repair(c, kEx, kKey, from, 1000, "completed", end);
        return plan_catchup(floor, frontier, kHorizon,
                            backfill_runs(c, kEx, kKey, *frontier - kHorizon, false));
    };

    // The full reach: what every periodic pass read before the fix, and what
    // the first candidate read whenever one repair pulled its bound to the floor.
    const Cost full = cost({{floor, kOpenEnded}});
    const Cost steady = cost(with_repairs({}).ranges);
    // VPS1's worst key in its worst pass (SOLUSDT, 2026-09-18): five recent
    // repairs. Spread here from three weeks to seven months back, plus three
    // below the floor, which the first candidate turned into a full reach.
    const auto vps1_worst = with_repairs({kDecember + 5 * kDay, kDecember + 30 * kDay,
                                          kDecember + 60 * kDay, kMarch + 10 * kDay,
                                          kMarch + 60 * kDay, kMarch + 110 * kDay,
                                          kMarch + 160 * kDay, kMarch + 186 * kDay});
    const Cost worst = cost(vps1_worst.ranges);
    // Four times VPS1's worst: twenty repairs across the whole history.
    std::vector<int64_t> twenty;
    for (int i = 0; i < 20; ++i) twenty.push_back(kMarch + (1 + 10 * i) * kDay);
    const Cost stress = cost(with_repairs(twenty).ranges);

    std::printf("PERF full_reach rows=%lld ms=%lld | steady rows=%lld ms=%lld | vps1_worst_key "
                "rows=%lld ms=%lld | 20_repairs rows=%lld ms=%lld\n",
                static_cast<long long>(full.rows), static_cast<long long>(full.ms),
                static_cast<long long>(steady.rows), static_cast<long long>(steady.ms),
                static_cast<long long>(worst.rows), static_cast<long long>(worst.ms),
                static_cast<long long>(stress.rows), static_cast<long long>(stress.ms));

    // Test 19: what a pass reads is exactly what its plan names, and the plan
    // is bounded by its ranges -- per timeframe, the recent range plus at most
    // 1000 minutes and two windows per repair -- not by the history.
    CHECK(vps1_worst.runs_below_floor == 3);
    CHECK_FALSE(vps1_worst.full_reach(floor));
    for (const Cost* k : {&steady, &worst, &stress}) {
        CHECK(k->rows == k->planned);
    }
    CHECK(full.rows == 207 * 1440 * 6);
    int64_t bound = 0;
    for (const auto& [tf, minutes] : tfs) bound += 5 * (1000 + 2 * minutes) + (35 + 2 * minutes);
    CAPTURE(bound, worst.rows);
    CHECK(worst.rows <= bound);
    CHECK(worst.rows * 50 < full.rows);
    // Test 20: time is statements x planning cost + rows x scan cost. Each
    // range is one statement per timeframe on a partitioned table, ~2 ms of
    // planning here before a row is read, so time falls less steeply than rows:
    // measured ~10x for VPS1's worst key (50x fewer rows), ~3.5x for twenty
    // repairs (13x fewer rows, 126 statements). Checked with margin, as ratios,
    // so they hold at any machine speed.
    CAPTURE(full.ms, worst.ms, stress.ms);
    CHECK(worst.ms * 4 < full.ms);
    CHECK(stress.ms * 2 < full.ms);
}
