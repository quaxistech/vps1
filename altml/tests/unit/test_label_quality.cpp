#include "altml/components/label_quality.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_CASE("label horizons reject a bad or missing source minute") {
    using altml::labels::SourceBar;
    std::vector<SourceBar> bars;
    constexpr int64_t start = 1'783'800'000'000;
    for (int minute = 0; minute <= 30; ++minute) {
        bars.push_back(SourceBar{.open_ms = start + minute * 60'000,
                                 .open = 100,
                                 .high = 101,
                                 .low = 99,
                                 .close = 100,
                                 .quality_ok = true});
    }
    CHECK(altml::labels::source_range_valid(bars, 0, 30));

    bars[7].quality_ok = false;
    CHECK(altml::labels::source_range_valid(bars, 0, 5));
    CHECK_FALSE(altml::labels::source_range_valid(bars, 0, 10));
    bars[7].quality_ok = true;
    bars[12].open_ms += 60'000;
    CHECK_FALSE(altml::labels::source_range_valid(bars, 0, 30));
}

TEST_CASE("ATR warmup resets at every quality failure and time gap") {
    using altml::labels::AtrState;
    using altml::labels::SourceBar;
    AtrState atr;
    constexpr int64_t start = 1'783'800'000'000;
    const auto feed = [&](int minute, bool quality_ok = true) {
        atr.advance(SourceBar{.open_ms = start + minute * 60'000,
                              .open = 100,
                              .high = 101,
                              .low = 99,
                              .close = 100,
                              .quality_ok = quality_ok});
    };

    for (int minute = 0; minute < 14; ++minute) feed(minute);
    CHECK(atr.ready());
    CHECK(atr.value() == 2.0);

    feed(14, false);
    CHECK_FALSE(atr.ready());
    for (int minute = 15; minute < 28; ++minute) feed(minute);
    CHECK_FALSE(atr.ready());
    feed(28);
    CHECK(atr.ready());

    feed(30);
    CHECK_FALSE(atr.ready());
}


namespace {
// UTC instants of the label store's calendar edges, as epoch milliseconds.
constexpr int64_t kFeb1st2026 = 1'769'904'000'000;     // 2026-02-01T00:00:00Z: AltSpotter's first candle
constexpr int64_t kOct23rd2026 = 1'792'713'600'000;    // 2026-10-23T00:00:00Z
constexpr int64_t kMar1st2028 = 1'835'481'600'000;     // 2028-03-01T00:00:00Z (after a leap day)
}

TEST_CASE("month_start_ms floors to the UTC month like altml.ensure_month_partitions") {
    using altml::labels::month_start_ms;
    CHECK(month_start_ms(kFeb1st2026) == kFeb1st2026);
    CHECK(month_start_ms(kFeb1st2026 + 27LL * 86'400'000 + 86'399'999) == kFeb1st2026);   // Feb 28 23:59:59.999
    CHECK(month_start_ms(kMar1st2028 - 1) == kMar1st2028 - 29LL * 86'400'000);             // Feb 29 2028 -> Feb 1
    CHECK(month_start_ms(kMar1st2028) == kMar1st2028);
    CHECK(month_start_ms(0) == 0);
}

TEST_CASE("every label the builder writes lies inside the provisioned partition window") {
    using namespace altml::labels;
    // Past, current and future instants; every month edge of three years,
    // the last and first millisecond around each, and a leap day.
    for (int64_t now = kFeb1st2026; now <= kMar1st2028 + 400LL * kOneDayMs; now += kOneDayMs / 2) {
        for (const int64_t at : {now, month_start_ms(now), month_start_ms(now) - 1, month_start_ms(now) + 1}) {
            if (at <= 0) continue;
            const auto window = label_partition_window(at);
            const int64_t floor = label_store_floor_ms(at, kLabelStoreDays);
            INFO("now=" << at);
            REQUIRE(window.from_ms <= floor);                        // the oldest label written has a month
            REQUIRE(window.from_ms == month_start_ms(window.from_ms));
            REQUIRE(window.to_ms >= at + kOneDayMs);                 // the newest closed bar and its horizon too
        }
    }
}

TEST_CASE("the initial ATR repair targets only a boundary the store can hold, and replays the same way") {
    using namespace altml::labels;
    const int64_t floor = label_store_floor_ms(kOct23rd2026, kLabelStoreDays);
    // The production case: history from 2026-02-01 predates every partition.
    CHECK_FALSE(initial_atr_repair_start_ms(kFeb1st2026, floor).has_value());
    // An instrument listed inside the store: its first candle is the boundary.
    CHECK(initial_atr_repair_start_ms(kOct23rd2026 - 3 * kOneDayMs, floor) == kOct23rd2026 - 3 * kOneDayMs);
    CHECK(initial_atr_repair_start_ms(floor, floor) == floor);              // the floor itself is held
    CHECK_FALSE(initial_atr_repair_start_ms(floor - 1, floor).has_value());
    CHECK_FALSE(initial_atr_repair_start_ms(std::nullopt, floor).has_value());   // no candle yet
    // A restart replays the same decision, and the floor only moves forward:
    // a boundary outside the store never comes back into it.
    for (int64_t later = kOct23rd2026; later <= kOct23rd2026 + 120 * kOneDayMs; later += kOneDayMs) {
        const int64_t later_floor = label_store_floor_ms(later, kLabelStoreDays);
        CHECK_FALSE(initial_atr_repair_start_ms(kFeb1st2026, later_floor).has_value());
        CHECK(initial_atr_repair_start_ms(kFeb1st2026, later_floor) == initial_atr_repair_start_ms(kFeb1st2026, later_floor));
    }
}
