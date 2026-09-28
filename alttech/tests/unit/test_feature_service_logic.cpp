#include "service_logic.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <set>

using namespace alttech::featsvc;

namespace {

alttech::talib_features::FBar replay_bar(int64_t open_ms, size_t sequence) {
    const double base = 100.0 + static_cast<double>(sequence) * 0.01;
    return {.open_time_ms = open_ms,
            .open = base,
            .high = base + 1.0,
            .low = base - 1.0,
            .close = base + 0.8,
            .base_volume = 1000.0 + static_cast<double>(sequence % 17),
            .quote_volume = base * 1000.0};
}

size_t core_feature_index(const std::string& name) {
    const auto& names = alttech::talib_features::core_feature_names();
    const auto it = std::find(names.begin(), names.end(), name);
    REQUIRE(it != names.end());
    return static_cast<size_t>(it - names.begin());
}

} // namespace

TEST_CASE("feature delivery does not retry a candle revision that PostgreSQL superseded") {
    CHECK(source_revision_state(8, 7) == SourceRevisionState::waiting_for_storage);
    CHECK(source_revision_state(8, 8) == SourceRevisionState::current);
    CHECK(source_revision_state(7, 8) == SourceRevisionState::stale);

    CHECK(source_barrier_decision(8, 7, false) ==
          SourceBarrierDecision::waiting_for_storage);
    CHECK(source_barrier_decision(8, 8, false) ==
          SourceBarrierDecision::waiting_for_storage);
    CHECK(source_barrier_decision(8, 8, true) == SourceBarrierDecision::current);
    CHECK(source_barrier_decision(7, 8, false) == SourceBarrierDecision::stale);
}

TEST_CASE("feature replay never sends an open-ended timestamp sentinel to PostgreSQL") {
    const auto minute = candle_query_plan(alttech::Venue::bitget, "1m", true, 5000);
    CHECK(minute.one_minute);
    CHECK(minute.open_ended);
    CHECK(minute.sql.find("to_timestamp($3") == std::string::npos);

    const auto aggregate = candle_query_plan(alttech::Venue::bitget, "15m", true, 5000);
    CHECK_FALSE(aggregate.one_minute);
    CHECK(aggregate.sql.find("timeframe = $3") != std::string::npos);
    CHECK(aggregate.sql.find("to_timestamp($3") == std::string::npos);

    const auto bounded = candle_query_plan(alttech::Venue::bitget, "15m", false, 100);
    CHECK(bounded.sql.find("to_timestamp($3") != std::string::npos);
    CHECK(bounded.sql.find("timeframe = $4") != std::string::npos);
}

TEST_CASE("the replay horizon bounds every timeframe, not only 1m") {
    constexpr int64_t day = 86'400'000;
    constexpr int64_t now = 40 * day + 1234;

    // Analytics can only be written where a partition exists, so the floor must
    // apply to every timeframe. Bounding only 1m would let the daily shard walk
    // back to the beginning of market history and fail on every write.
    for (const auto timeframe : {alttech::Timeframe::m1, alttech::Timeframe::m15,
                                 alttech::Timeframe::h1, alttech::Timeframe::d1}) {
        CHECK(bounded_replay_floor_ms(timeframe, 20, now) == 20 * day + 1234);
    }
    CHECK(bounded_replay_floor_ms(alttech::Timeframe::m1, 0, now) == 0);
    // A horizon reaching past the epoch has no floor to apply.
    CHECK(bounded_replay_floor_ms(alttech::Timeframe::m1, 50, now) == 0);

    const auto minute = missing_vector_query_plan(alttech::Venue::bitget, "1m");
    CHECK(minute.one_minute);
    CHECK(minute.sql.find("market_candles_1m") != std::string::npos);
    CHECK(minute.sql.find("open_time >= to_timestamp($5") != std::string::npos);

    const auto aggregate = missing_vector_query_plan(alttech::Venue::bitget, "15m");
    CHECK_FALSE(aggregate.one_minute);
    CHECK(aggregate.sql.find("market_candles_agg") != std::string::npos);
    CHECK(aggregate.sql.find("c.timeframe = $2") != std::string::npos);
    CHECK(aggregate.sql.find("open_time >= to_timestamp($5") != std::string::npos);
}

TEST_CASE("feature work is coalesced per shard without losing bootstrap backfill") {
    PendingTask pending{TaskMode::live, -2, 0};
    merge_pending_task(pending, PendingTask{TaskMode::backfill, -1, 0});
    CHECK(pending.mode == TaskMode::backfill);

    merge_pending_task(pending, PendingTask{TaskMode::recompute, 3000, 1});
    CHECK(pending.mode == TaskMode::backfill);
    CHECK(pending.from_ms == -1);
    CHECK(pending.attempt == 1);

    pending = PendingTask{TaskMode::recompute, 3000, 1};
    merge_pending_task(pending, PendingTask{TaskMode::recompute, 1000, 0});
    CHECK(pending.mode == TaskMode::recompute);
    CHECK(pending.from_ms == 1000);

    merge_pending_task(pending, PendingTask{TaskMode::live, -2, 4});
    CHECK(pending.mode == TaskMode::recompute);
    CHECK(pending.from_ms == 1000);
    CHECK(pending.attempt == 4);

    merge_pending_task(pending, PendingTask{TaskMode::backfill, -1, 0});
    CHECK(pending.mode == TaskMode::backfill);
    CHECK(pending.from_ms == -1);
}

TEST_CASE("feature replay resets warmup at every source gap") {
    constexpr int64_t span = 60'000;
    alttech::talib_features::FeatureEngine engine;
    ReplayCursor cursor;
    ReplayStep step;

    // Enough contiguous history for the fast indicator ladder to converge past
    // kMinUsableFraction. Measured on this series: 40 bars carry 59.3% of the
    // features and 50 carry 66.1%, so kBarsToValid sits comfortably above the
    // threshold without depending on its exact value.
    constexpr size_t kBarsToValid = 60;

    for (size_t i = 0; i < 10; ++i) {
        step = process_contiguous_replay_bar(
            engine, cursor, span, replay_bar(static_cast<int64_t>(i) * span, i));
        CHECK_FALSE(step.reset_at_gap);
    }
    // Ten bars is far too little for the ladder: WARMING regardless of period.
    REQUIRE(step.vector.status == alttech::talib_features::VectorStatus::warming);

    for (size_t i = 10; i < kBarsToValid; ++i) {
        step = process_contiguous_replay_bar(
            engine, cursor, span, replay_bar(static_cast<int64_t>(i) * span, i));
        CHECK_FALSE(step.reset_at_gap);
    }
    REQUIRE(step.vector.status == alttech::talib_features::VectorStatus::valid);

    // A missing candle discards the context: the next bar starts a fresh
    // series and cannot be VALID on its own, whatever the previous run held.
    const int64_t after_gap = cursor.last_source_open_ms + 2 * span;
    step = process_contiguous_replay_bar(
        engine, cursor, span, replay_bar(after_gap, kBarsToValid));
    CHECK(step.reset_at_gap);
    CHECK(step.vector.status == alttech::talib_features::VectorStatus::warming);
    CHECK(engine.bars_seen() == 1);

    for (size_t i = 1; i < kBarsToValid; ++i) {
        step = process_contiguous_replay_bar(
            engine, cursor, span,
            replay_bar(after_gap + static_cast<int64_t>(i) * span, kBarsToValid + i));
    }
    CHECK(step.vector.status == alttech::talib_features::VectorStatus::valid);
    CHECK(engine.bars_seen() == kBarsToValid);

    CHECK_THROWS_AS(process_contiguous_replay_bar(
                        engine, cursor, span,
                        replay_bar(cursor.last_source_open_ms, 999)),
                    std::invalid_argument);
}

TEST_CASE("bad-quality candle is explicit and restarts contiguous warmup") {
    constexpr int64_t span = 60'000;
    alttech::talib_features::FeatureEngine engine;
    ReplayCursor cursor;
    ReplayStep step;
    constexpr size_t kBarsToValid = 60;

    for (size_t i = 0; i < kBarsToValid; ++i) {
        step = process_contiguous_replay_bar(
            engine, cursor, span, replay_bar(static_cast<int64_t>(i) * span, i));
    }
    REQUIRE(step.vector.status == alttech::talib_features::VectorStatus::valid);

    // A bad-quality bar is reported as such AND clears the incremental window:
    // its OHLC cannot be trusted to carry the series forward.
    step = process_contiguous_replay_bar(
        engine, cursor, span,
        replay_bar(static_cast<int64_t>(kBarsToValid) * span, kBarsToValid),
        false);
    CHECK(step.invalid_input);
    CHECK(step.vector.status == alttech::talib_features::VectorStatus::incomplete_input);
    CHECK(engine.bars_seen() == 0);

    // A handful of bars after the reset is not enough to converge the ladder
    // again: still WARMING, because the fraction of usable features is low.
    for (size_t i = 1; i <= 10; ++i) {
        step = process_contiguous_replay_bar(
            engine, cursor, span,
            replay_bar(static_cast<int64_t>(kBarsToValid + i) * span, kBarsToValid + i));
    }
    CHECK(step.vector.status == alttech::talib_features::VectorStatus::warming);

    // Rebuilding the full run restores VALID.
    for (size_t i = 11; i < kBarsToValid; ++i) {
        step = process_contiguous_replay_bar(
            engine, cursor, span,
            replay_bar(static_cast<int64_t>(kBarsToValid + i) * span, kBarsToValid + i));
    }
    CHECK(step.vector.status == alttech::talib_features::VectorStatus::valid);
}

TEST_CASE("feature engine restores the incremental window from a DB replay") {
    constexpr int64_t span = 60'000;
    constexpr size_t history_size =
        alttech::talib_features::FeatureEngine::kMinValidBars + 10;

    std::vector<alttech::talib_features::FBar> history;
    history.reserve(history_size + 1);
    for (size_t i = 0; i < history_size; ++i) {
        history.push_back(replay_bar(static_cast<int64_t>(i) * span, i));
    }
    const auto next = replay_bar(static_cast<int64_t>(history_size) * span, history_size);

    alttech::talib_features::FeatureEngine restored;
    REQUIRE(restored.restore_state(history, span));
    CHECK(restored.bars_seen() == history.size());
    CHECK(restored.last_open_time_ms() == history.back().open_time_ms);

    auto full_series = history;
    full_series.push_back(next);
    const auto expected = alttech::talib_features::FeatureEngine::process_series(full_series);
    REQUIRE(expected.size() == full_series.size());

    const auto actual = restored.process_bar(next);
    CHECK(actual.bar_open_time_ms == expected.back().bar_open_time_ms);
    CHECK(actual.status == expected.back().status);
    CHECK(actual.validity == expected.back().validity);
    CHECK(actual.values == expected.back().values);
    CHECK(restored.bars_seen() == history.size() + 1);
}

TEST_CASE("feature state restore rejects non-contiguous input without mutating live state") {
    constexpr int64_t span = 60'000;
    alttech::talib_features::FeatureEngine engine;
    const std::vector initial{replay_bar(0, 0), replay_bar(span, 1)};
    REQUIRE(engine.restore_state(initial, span));

    const std::vector invalid{replay_bar(2 * span, 2), replay_bar(4 * span, 3)};
    const auto result = engine.restore_state(invalid, span);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == alttech::ErrorCode::invalid_argument);
    CHECK(engine.bars_seen() == initial.size());
    CHECK(engine.last_open_time_ms() == span);

    REQUIRE(engine.restore_state(std::span<const alttech::talib_features::FBar>{}, span));
    CHECK(engine.bars_seen() == 0);
    CHECK(engine.last_open_time_ms() == 0);
}

TEST_CASE("cumulative A/D is invariant under bounded restart replay") {
    constexpr int64_t span = 60'000;
    constexpr size_t total = alttech::talib_features::FeatureEngine::kWarmupBars + 300;
    alttech::talib_features::FeatureEngine uninterrupted;
    alttech::talib_features::FeatureVector uninterrupted_last;
    for (size_t i = 0; i < total; ++i) {
        uninterrupted_last =
            uninterrupted.process_bar(replay_bar(static_cast<int64_t>(i) * span, i));
    }

    alttech::talib_features::FeatureEngine restarted;
    alttech::talib_features::FeatureVector restarted_last;
    const size_t replay_from = total - alttech::talib_features::FeatureEngine::kWarmupBars;
    for (size_t i = replay_from; i < total; ++i) {
        restarted_last = restarted.process_bar(replay_bar(static_cast<int64_t>(i) * span, i));
    }

    const size_t slope = core_feature_index("AD_outReal");
    REQUIRE(uninterrupted_last.is_valid(slope));
    REQUIRE(restarted_last.is_valid(slope));
    CHECK(uninterrupted_last.values[slope] == restarted_last.values[slope]);
}

// -----------------------------------------------------------------------------
// P0 regression: a candle batch that STRADDLES the replay floor.
//
// The module clamped what it WROTE to the floor but still checked completeness
// from the batch's original oldest bar. A vector for a bar below the floor can
// never exist, so the delivery was retried forever. In production this pinned
// 275 messages in permanent redelivery on ALTSPOTTER_BARS, saturated the
// consumer's ack-pending budget so 518 live bars queued behind them were never
// processed, and left AltTech publishing no feature vector for 6.8 days — which
// starved AltStrategy of context and stopped the entire VPS1 chain, with every
// service still reporting itself healthy.
// -----------------------------------------------------------------------------
TEST_CASE("batch_processing_range: a batch straddling the floor is clamped, never refused") {
    constexpr int64_t floor = 1'000'000;

    SECTION("entirely below the floor is nothing to do") {
        const auto r = batch_processing_range(500'000, 900'000, floor);
        CHECK_FALSE(r.actionable);
    }

    SECTION("entirely above the floor is untouched") {
        const auto r = batch_processing_range(2'000'000, 3'000'000, floor);
        CHECK(r.actionable);
        CHECK_FALSE(r.clamped);
        CHECK(r.from_ms == 2'000'000);
    }

    SECTION("straddling the floor is actionable, and starts AT the floor") {
        // This is the case that hung the pipeline: it must not be refused, and
        // it must not start below the floor.
        const auto r = batch_processing_range(500'000, 2'000'000, floor);
        CHECK(r.actionable);
        CHECK(r.clamped);
        CHECK(r.from_ms == floor);
        // The decisive property: the range never begins where nothing can be
        // written, so the completeness check it feeds can actually be satisfied.
        CHECK(r.from_ms >= floor);
    }

    SECTION("a batch touching the floor exactly is actionable") {
        const auto r = batch_processing_range(floor, floor, floor);
        CHECK(r.actionable);
        CHECK_FALSE(r.clamped);
        CHECK(r.from_ms == floor);
    }

    SECTION("no configured horizon leaves the batch alone") {
        const auto r = batch_processing_range(1, 5, 0);
        CHECK(r.actionable);
        CHECK_FALSE(r.clamped);
        CHECK(r.from_ms == 1);
    }

    SECTION("a malformed range is refused rather than guessed") {
        CHECK_FALSE(batch_processing_range(5, 1, floor).actionable);
    }

    SECTION("every actionable range is storable — the invariant that was violated") {
        for (int64_t earliest = 0; earliest <= 2'000'000; earliest += 250'000) {
            for (int64_t latest = earliest; latest <= 2'500'000; latest += 500'000) {
                const auto r = batch_processing_range(earliest, latest, floor);
                if (!r.actionable) continue;
                INFO("earliest=" << earliest << " latest=" << latest);
                CHECK(r.from_ms >= floor);
                CHECK(r.from_ms <= latest);
            }
        }
    }
}

// Regression: P0-3. Any incomplete bar used to wipe the entire warm-up window,
// which made a VALID vector unreachable on 15m and above and silently starved
// the whole decision chain below AltTech.
TEST_CASE("an incomplete bar keeps the warm-up window, a missing bar does not",
          "[feature][service_logic][regression]") {
    SECTION("a gap discards the context") {
        REQUIRE_FALSE(warmup_context_survives(true, true));
        REQUIRE_FALSE(warmup_context_survives(true, false));
    }

    SECTION("an incomplete bar does not") {
        REQUIRE(warmup_context_survives(false, false));
        REQUIRE(warmup_context_survives(false, true));
    }

    // The measured shape of this deployment: a 15m series where roughly a
    // quarter of the bars are incomplete but none are absent. Replaying it
    // through the rule must accumulate enough contiguous context to publish,
    // which is exactly what the pre-fix behaviour could not do.
    SECTION("a real incomplete-heavy series still reaches the valid threshold") {
        constexpr int kMinValidBars = 250;
        constexpr int kBars = 960;

        int survived_worst_case = 0;   // fix in place
        int survived_old_rule = 0;     // previous behaviour, for contrast
        int longest_old_run = 0;

        for (int i = 0; i < kBars; ++i) {
            const bool incomplete = (i % 4) == 0;   // 25%, no gaps at all
            const bool gap = false;

            survived_worst_case = warmup_context_survives(gap, !incomplete)
                                      ? survived_worst_case + 1
                                      : 0;
            survived_old_rule = (gap || incomplete) ? 0 : survived_old_rule + 1;
            longest_old_run = std::max(longest_old_run, survived_old_rule);
        }

        REQUIRE(survived_worst_case >= kMinValidBars);
        REQUIRE(longest_old_run < kMinValidBars);   // the defect, reproduced
    }

    SECTION("a genuine gap still resets the accumulated window") {
        int survived = 0;
        for (int i = 0; i < 400; ++i) {
            const bool gap = i == 300;
            survived = warmup_context_survives(gap, true) ? survived + 1 : 0;
        }
        REQUIRE(survived == 99);   // 301..399, counted after the reset at 300
    }
}

// Regression: P0-3, second half. The under-filled bar now stays in the
// indicator series so the window around it stays time-contiguous, which makes
// it possible for the bar itself to be published as VALID by accident.
TEST_CASE("an under-filled bar never publishes as a normal vector",
          "[feature][service_logic][regression]") {
    REQUIRE(published_vector_quality(true, true) == "VALID");
    REQUIRE(published_vector_quality(true, false) == "WARMING");

    // Both of these are the defect: a warm window does not make an
    // under-filled bar's own volume-weighted features trustworthy.
    REQUIRE(published_vector_quality(false, true) == "INCOMPLETE_INPUT");
    REQUIRE(published_vector_quality(false, false) == "INCOMPLETE_INPUT");
}

// Regression: a cross-consumer storage barrier with no exit. One 30m batch was
// redelivered 1,168 times in 20 minutes waiting for a PostgreSQL state that was
// never going to arrive, holding a slot in the ack-pending budget the whole
// time — the same shape as the P0-1 stall, reached by a different route.
TEST_CASE("a storage barrier stops waiting once it is divergence, not lag",
          "[feature][service_logic][regression]") {
    constexpr int64_t kBound = 20;

    SECTION("normal writer lag is still waited out") {
        REQUIRE_FALSE(storage_barrier_exhausted(1, kBound));
        REQUIRE_FALSE(storage_barrier_exhausted(19, kBound));
        REQUIRE_FALSE(storage_barrier_exhausted(20, kBound));
    }

    SECTION("past the bound it is a divergence") {
        REQUIRE(storage_barrier_exhausted(21, kBound));
        REQUIRE(storage_barrier_exhausted(1168, kBound));   // the production count
    }

    SECTION("no configured bound keeps the previous unbounded behaviour") {
        REQUIRE_FALSE(storage_barrier_exhausted(1168, 0));
        REQUIRE_FALSE(storage_barrier_exhausted(1168, -1));
    }

    // A message with no JetStream metadata reports 0 deliveries; it must never
    // be mistaken for an exhausted barrier.
    SECTION("a message without metadata is not exhausted") {
        REQUIRE_FALSE(storage_barrier_exhausted(0, kBound));
        REQUIRE_FALSE(storage_barrier_exhausted(0, 1));
    }

    // The bound has to be reachable: with the observed ~1 redelivery per second
    // the default retires a stuck message in well under a minute, where the old
    // behaviour never retired it at all.
    SECTION("the default bound terminates in bounded time") {
        int deliveries = 0;
        while (!storage_barrier_exhausted(static_cast<uint64_t>(deliveries), kBound)) {
            ++deliveries;
            REQUIRE(deliveries < 1000);
        }
        REQUIRE(deliveries == kBound + 1);
    }
}

// Regression: a sparse batch verified through a ranged query with a LIMIT sized
// by the batch. Instrument 25, 30m, a batch spanning 08-21 23:30 to 08-24 01:30
// — fifty hours of bar slots — was redelivered 8,599 times because the bar at
// the far end of that span fell outside its own query's LIMIT and therefore
// looked unstored.
TEST_CASE("a batch is verified by exact bars, not by a prefix of their range",
          "[feature][service_logic][regression]") {
    SECTION("the query names the bars instead of scanning the window") {
        const std::string sql = candles_at_query(alttech::Venue::bitget, "30m");
        // No LIMIT can truncate what is asked for by identity.
        REQUIRE(sql.find("LIMIT") == std::string::npos);
        REQUIRE(sql.find("unnest($2::bigint[])") != std::string::npos);
        REQUIRE(sql.find("timeframe = $3") != std::string::npos);
    }

    SECTION("1m reads its own table and needs no timeframe predicate") {
        const std::string sql = candles_at_query(alttech::Venue::bitget, "1m");
        REQUIRE(sql.find("market_candles_1m") != std::string::npos);
        REQUIRE(sql.find("market_candles_agg") == std::string::npos);
        REQUIRE(sql.find("timeframe = $3") == std::string::npos);
        REQUIRE(sql.find("LIMIT") == std::string::npos);
    }

    SECTION("aggregate timeframes read the aggregate table") {
        for (const char* tf : {"5m", "15m", "30m", "1h", "4h"}) {
            const std::string sql = candles_at_query(alttech::Venue::bitget, tf);
            REQUIRE(sql.find("market_candles_agg") != std::string::npos);
            REQUIRE(sql.find("market_candles_1m") == std::string::npos);
        }
    }

    // The shape that broke: the old sizing rule against the real batch.
    SECTION("the old ranged sizing could not have covered that batch") {
        constexpr int64_t kSpan = 30LL * 60 * 1000;
        constexpr int64_t kFirst = 1787355000000;   // 08-21 23:30
        constexpr int64_t kLast  = 1787535000000;   // 08-24 01:30
        const int64_t slots_in_range = (kLast - kFirst) / kSpan + 1;

        // A sparse batch: the two ends and a handful between them.
        const int64_t batch_size = 5;
        const int64_t old_limit = batch_size + 1;

        REQUIRE(slots_in_range == 101);
        REQUIRE(old_limit < slots_in_range);   // the final bar was unreachable
    }
}

TEST_CASE("every candle query is scoped to its own venue") {
    // The feature engine reads one venue's candles per shard. When these query
    // builders hardcoded `exchange_id = 1`, every shard — whichever venue it
    // belonged to — read Bitget's candles, and all four venues produced
    // byte-identical indicator values from the same series. The bug was
    // invisible in the vector counts, which looked correct, and only showed up
    // when the same ATR appeared under four different exchange ids.
    using alttech::Venue;
    // Driven by the venue registry, not a copy of it: a venue added to
    // kAllVenues must be covered here without anyone remembering to edit this.
    const auto& venues = alttech::kAllVenues;

    std::set<std::string> plans, at_queries, missing_plans;
    for (const auto v : venues) {
        plans.insert(candle_query_plan(v, "1m", true, 5000).sql);
        plans.insert(candle_query_plan(v, "15m", false, 100).sql);
        at_queries.insert(candles_at_query(v, "1m"));
        missing_plans.insert(missing_vector_query_plan(v, "5m").sql);
    }
    // Two timeframes per venue. Any collapse means a venue is reading someone
    // else's candles.
    CHECK(plans.size() == venues.size() * 2);
    CHECK(at_queries.size() == venues.size());
    CHECK(missing_plans.size() == venues.size());

    // And each names its own exchange id explicitly.
    CHECK(candle_query_plan(Venue::bitget, "1m", true, 10).sql.find("exchange_id = 1") !=
          std::string::npos);
    CHECK(candle_query_plan(Venue::binance_usdm, "1m", true, 10).sql.find("exchange_id = 2") !=
          std::string::npos);
    CHECK(candle_query_plan(Venue::okx, "1m", true, 10).sql.find("exchange_id = 3") !=
          std::string::npos);
    CHECK(candle_query_plan(Venue::bybit, "1m", true, 10).sql.find("exchange_id = 4") !=
          std::string::npos);
}

// A vector's status must reflect how much of it converged, not how long the
// series is. Measured on VPS1 before this rule existed: a 1d vector built from
// ~75 bars carried 733 of 995 features and was published WARMING, so AltML —
// which copies only `status = 'VALID'` — discarded three quarters of a usable
// vector because a handful of period-200 T3 outputs (lookback ~1300 bars) had
// not converged.
TEST_CASE("vector status follows the usable fraction, not the bar count") {
    constexpr int64_t span = 60'000;
    const auto series = [](size_t n) {
        std::vector<alttech::talib_features::FBar> bars;
        bars.reserve(n);
        for (size_t i = 0; i < n; ++i) bars.push_back(replay_bar(static_cast<int64_t>(i) * span, i));
        return bars;
    };
    const size_t total = alttech::talib_features::core_feature_names().size();
    const auto usable = [&](const alttech::talib_features::FeatureVector& v) {
        size_t n = 0;
        for (size_t i = 0; i < total; ++i) {
            if (v.is_valid(i)) ++n;
        }
        return static_cast<double>(n) / static_cast<double>(total);
    };

    SECTION("a nearly empty series is WARMING") {
        const auto v = alttech::talib_features::FeatureEngine::process_series(series(15));
        REQUIRE_FALSE(v.empty());
        CHECK(usable(v.back()) < alttech::talib_features::FeatureEngine::kMinUsableFraction);
        CHECK(v.back().status == alttech::talib_features::VectorStatus::warming);
    }

    SECTION("the fast ladder converging makes it VALID, long lookbacks or not") {
        const auto v = alttech::talib_features::FeatureEngine::process_series(series(60));
        REQUIRE_FALSE(v.empty());
        CHECK(usable(v.back()) >= alttech::talib_features::FeatureEngine::kMinUsableFraction);
        CHECK(v.back().status == alttech::talib_features::VectorStatus::valid);
        // The point of the rule: slow features are STILL invalid here, and that
        // no longer blocks the vector.
        CHECK(usable(v.back()) < 1.0);
    }

    SECTION("status and bitmap agree at every length") {
        for (size_t n : {15U, 40U, 60U, 120U, 300U}) {
            const auto v = alttech::talib_features::FeatureEngine::process_series(series(n));
            REQUIRE_FALSE(v.empty());
            const bool expect_valid =
                n >= alttech::talib_features::FeatureEngine::kMinValidBars &&
                usable(v.back()) >= alttech::talib_features::FeatureEngine::kMinUsableFraction;
            CHECK((v.back().status == alttech::talib_features::VectorStatus::valid) == expect_valid);
        }
    }
}

// Accumulating indicators (AD, ADOSC, OBV) accumulate from the first bar of the
// series, and the engine feeds a SLIDING window. TA-Lib returns a finite value
// from the very first bar, so the ordinary "finite means valid" rule published
// them long before the level meant anything. Measured on one real instrument,
// the same bar gave AD = 4.2M at 100 bars, 11.5M at 200, 5.2M at 400 and 22.5M
// at 2048 — the level described how much history was loaded, not the market.
// OBV is one of the twelve features AltML's t0_projection reads.
TEST_CASE("accumulating indicators are invalid until the window is full") {
    constexpr int64_t span = 60'000;
    const auto series = [](size_t n) {
        std::vector<alttech::talib_features::FBar> bars;
        bars.reserve(n);
        for (size_t i = 0; i < n; ++i) bars.push_back(replay_bar(static_cast<int64_t>(i) * span, i));
        return bars;
    };
    const auto& names = alttech::talib_features::core_feature_names();
    const auto index_of = [&](std::string_view want) {
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == want) return i;
        }
        FAIL("feature not in catalog: " << want);
        return size_t{0};
    };
    const size_t ad = index_of("AD_outReal");
    const size_t obv = index_of("OBV_outReal");
    const size_t adosc = index_of("ADOSC_outReal");
    const size_t rsi = index_of("RSI_14_outReal");
    constexpr size_t full = alttech::talib_features::FeatureEngine::kWarmupBars;

    SECTION("a partial window leaves them invalid while ordinary features publish") {
        const auto v = alttech::talib_features::FeatureEngine::process_series(series(500));
        REQUIRE_FALSE(v.empty());
        CHECK_FALSE(v.back().is_valid(ad));
        CHECK_FALSE(v.back().is_valid(obv));
        CHECK_FALSE(v.back().is_valid(adosc));
        // A fixed-lookback indicator is unaffected by this rule.
        CHECK(v.back().is_valid(rsi));
    }

    SECTION("one bar short of a full window is still invalid") {
        const auto v = alttech::talib_features::FeatureEngine::process_series(series(full - 1));
        REQUIRE_FALSE(v.empty());
        CHECK_FALSE(v.back().is_valid(ad));
        CHECK_FALSE(v.back().is_valid(obv));
    }

    SECTION("a full window makes them valid") {
        const auto v = alttech::talib_features::FeatureEngine::process_series(series(full));
        REQUIRE_FALSE(v.empty());
        CHECK(v.back().is_valid(ad));
        CHECK(v.back().is_valid(obv));
        CHECK(v.back().is_valid(adosc));
    }

    SECTION("an invalid accumulator stores a benign zero, never a stale level") {
        const auto v = alttech::talib_features::FeatureEngine::process_series(series(500));
        REQUIRE_FALSE(v.empty());
        CHECK(v.back().values[ad] == 0.0F);
        CHECK(v.back().values[obv] == 0.0F);
    }
}
