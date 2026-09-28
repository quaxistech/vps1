#include "altmacro/domain/validation.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace altmacro::domain;

namespace {
PriceBar bar(int64_t ms, double o, double h, double l, double c) {
    PriceBar b;
    b.timeframe = "1m"; b.event_time_ms = ms;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.quality = QualityStatus::valid;
    return b;
}
} // namespace

TEST_CASE("a clean series passes untouched", "[validation]") {
    std::vector<PriceBar> bars{bar(1000, 1.1, 1.2, 1.0, 1.15), bar(2000, 1.15, 1.3, 1.1, 1.2)};
    SanityBand band{0.5, 2.0};
    auto r = validate_bars(bars, band);
    CHECK(r.clean());
    CHECK(r.valid == 2);
    CHECK(bars.size() == 2);
}

TEST_CASE("duplicate timestamps are dropped rather than merged", "[validation]") {
    // Two rows claiming the same instant cannot both be right, and this layer
    // has no basis on which to choose between them.
    std::vector<PriceBar> bars{bar(1000, 1.1, 1.2, 1.0, 1.15), bar(1000, 1.4, 1.5, 1.3, 1.45)};
    SanityBand band{0.5, 2.0};
    auto r = validate_bars(bars, band);
    CHECK(r.rejected == 1);
    CHECK(bars.size() == 1);
    CHECK(bars.front().open.value() == 1.1);
}

TEST_CASE("out-of-order rows are rejected", "[validation]") {
    std::vector<PriceBar> bars{bar(2000, 1.1, 1.2, 1.0, 1.15), bar(1000, 1.1, 1.2, 1.0, 1.15)};
    SanityBand band{0.5, 2.0};
    auto r = validate_bars(bars, band);
    CHECK(r.rejected == 1);
    CHECK(bars.size() == 1);
}

TEST_CASE("a zero price is rejected, never stored", "[validation]") {
    std::vector<PriceBar> bars{bar(1000, 0.0, 1.2, 0.0, 1.15)};
    SanityBand band{0.5, 2.0};
    auto r = validate_bars(bars, band);
    CHECK(r.rejected == 1);
    CHECK(bars.empty());
}

TEST_CASE("a market-closed marker survives validation with no price", "[validation]") {
    // The marker is information: it records that the source said this period
    // held nothing, which a consumer cannot reconstruct from a hole.
    PriceBar closed;
    closed.timeframe = "1m"; closed.event_time_ms = 1000;
    closed.quality = QualityStatus::market_closed;
    std::vector<PriceBar> bars{closed};
    SanityBand band{0.5, 2.0};
    auto r = validate_bars(bars, band);
    CHECK(r.clean());
    CHECK(r.market_closed == 1);
    CHECK(bars.size() == 1);
    CHECK_FALSE(bars.front().close.has_value());
}

TEST_CASE("a ten-times scale error is caught by the sanity band", "[validation][scale]") {
    // Internally perfect, externally absurd. Without the band this passes every
    // ordering check and is discovered months later by a model that learned it.
    std::vector<PriceBar> bars{bar(1000, 67.0964, 67.0978, 67.0960, 67.0970)};
    SanityBand usdcnh{5.0, 9.0};
    auto r = validate_bars(bars, usdcnh);
    CHECK(r.rejected == 1);
    CHECK(bars.empty());
    CHECK(r.issues.front().rule == "outside_sanity_band");
}

TEST_CASE("a crossed book is reported", "[validation][bidask]") {
    std::vector<PriceBar> bid{bar(1000, 1.2, 1.2, 1.2, 1.2)};
    std::vector<PriceBar> ask{bar(1000, 1.1, 1.1, 1.1, 1.1)};
    auto r = validate_bid_ask(bid, ask);
    CHECK(r.rejected == 1);
    CHECK(r.issues.front().rule == "crossed_book");
}

TEST_CASE("a normal book passes", "[validation][bidask]") {
    std::vector<PriceBar> bid{bar(1000, 1.1, 1.1, 1.1, 1.1)};
    std::vector<PriceBar> ask{bar(1000, 1.2, 1.2, 1.2, 1.2)};
    auto r = validate_bid_ask(bid, ask);
    CHECK(r.clean());
    CHECK(r.valid == 1);
}

TEST_CASE("a reference row marked absent must not carry a number", "[validation][reference]") {
    ReferenceValue v;
    v.event_time_ms = 1000;
    v.quality = QualityStatus::missing;
    v.value = 0.0;   // exactly the mistake this service exists to prevent
    std::vector<ReferenceValue> values{v};
    SanityBand band{0.0, 100.0};
    auto r = validate_reference(values, band);
    CHECK(r.rejected == 1);
    CHECK(values.empty());
}

TEST_CASE("a negative yield is accepted", "[validation][reference]") {
    // Rates go below zero and that is a real market state, not a defect: the
    // band, not a positivity rule, is what constrains a reference series.
    ReferenceValue v;
    v.event_time_ms = 1000;
    v.quality = QualityStatus::valid;
    v.value = -0.35;
    std::vector<ReferenceValue> values{v};
    SanityBand band{-5.0, 25.0};
    auto r = validate_reference(values, band);
    CHECK(r.clean());
    CHECK(values.size() == 1);
}
