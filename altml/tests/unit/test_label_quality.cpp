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

