// alttech-dataset: the historical reconstruction must aggregate and emit
// exactly as the live path does, or a model trained on it learns other numbers.

#include "../../src/tools/dataset/dataset_logic.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace alttech::dataset;

namespace {
MinuteBar m(int64_t minute, double o, double h, double l, double c, double v = 1) {
    return {minute * 60'000, o, h, l, c, v, v * c, true};
}
} // namespace

TEST_CASE("aggregation follows AltSpotter: first open, extremes, last close, sums",
          "[dataset]") {
    const std::vector<MinuteBar> in = {m(0, 10, 11, 9, 10.5, 2), m(1, 10.5, 12, 10, 11, 3),
                                       m(2, 11, 11.5, 8, 9, 1), m(3, 9, 9.5, 8.5, 9.2, 4),
                                       m(4, 9.2, 10, 9, 9.8, 5)};
    const auto out = aggregate(in, 5);
    REQUIRE(out.size() == 1);
    CHECK(out[0].bar.open_time_ms == 0);
    CHECK(out[0].bar.open == 10);
    CHECK(out[0].bar.high == 12);
    CHECK(out[0].bar.low == 8);
    CHECK(out[0].bar.close == 9.8);
    CHECK(out[0].bar.base_volume == 15);
    CHECK(out[0].ok);
}

TEST_CASE("a window missing a minute is incomplete, and windows are epoch-aligned UTC",
          "[dataset]") {
    // Minutes 3..7: window [0,5) holds 3,4 (incomplete), window [5,10) holds 5,6,7.
    const std::vector<MinuteBar> in = {m(3, 1, 1, 1, 1), m(4, 1, 1, 1, 1), m(5, 2, 2, 2, 2),
                                       m(6, 2, 2, 2, 2), m(7, 2, 2, 2, 2)};
    const auto out = aggregate(in, 5);
    REQUIRE(out.size() == 2);
    CHECK(out[0].bar.open_time_ms == 0);
    CHECK_FALSE(out[0].ok);
    CHECK(out[1].bar.open_time_ms == 5 * 60'000);
    CHECK_FALSE(out[1].ok);   // 3 of 5 minutes
}

TEST_CASE("1m passes through with the candle's own quality", "[dataset]") {
    auto bad = m(1, 1, 1, 1, 1);
    bad.ok = false;
    const auto out = aggregate({m(0, 1, 1, 1, 1), bad}, 1);
    REQUIRE(out.size() == 2);
    CHECK(out[0].ok);
    CHECK_FALSE(out[1].ok);
}

TEST_CASE("emission is on bar CLOSE times of the decision grid", "[dataset]") {
    const int64_t min = 60'000;
    // 1m bars on a 15-minute grid: the bar opening at :14 closes at :15.
    CHECK(emit_bar(14 * min, min, 15 * min, 0, INT64_MAX));
    CHECK_FALSE(emit_bar(15 * min, min, 15 * min, 0, INT64_MAX));
    // A 1h bar is emitted whenever it closes (every close is on a 15m grid).
    CHECK(emit_bar(60 * min, 60 * min, 15 * min, 0, INT64_MAX));
    // Range bounds apply to the open time.
    CHECK_FALSE(emit_bar(14 * min, min, 15 * min, 20 * min, INT64_MAX));
}
