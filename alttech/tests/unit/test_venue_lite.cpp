// Per-venue reference engine tests.
#include "reference_engine.hpp"
#include "reference_features.hpp"

#include <ta_libc.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace alttech::venuelite;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::vector<LiteCandle> ramp(std::size_t n, double start, double step) {
    std::vector<LiteCandle> w;
    w.reserve(n);
    double p = start;
    for (std::size_t i = 0; i < n; ++i) {
        LiteCandle c;
        c.open_time_ms = static_cast<int64_t>(i) * 60'000;
        c.open = p;
        c.close = p + step;
        c.high = std::max(c.open, c.close) + 0.1;
        c.low = std::min(c.open, c.close) - 0.1;
        c.base_volume = 100.0;
        c.quote_volume = 100.0 * p;
        p = c.close;
        w.push_back(c);
    }
    return w;
}

bool valid(const RefResult& r, std::size_t idx) {
    return (r.validity[idx / 8] >> (idx % 8)) & 1U;
}

} // namespace

TEST_CASE("reference engine computes a rising trend with bullish EMA alignment") {
    auto w = ramp(220, 100.0, 0.05); // steady uptrend
    LiteContext ctx;
    ctx.data_quality = 1.0;
    const auto r = compute_reference(w, ctx);

    REQUIRE(valid(r, RF_RETURN));
    CHECK(r.values[RF_RETURN] > 0.0);
    REQUIRE(valid(r, RF_EMA_ALIGNMENT));
    CHECK(r.values[RF_EMA_ALIGNMENT] == 1.0F);       // 20 > 50 > 200
    REQUIRE(valid(r, RF_RSI_14));
    CHECK(r.values[RF_RSI_14] > 55.0F);              // persistent gains
    CHECK_FALSE(r.warming);                          // 220 >= 200 bars
    // A market carried far beyond its own noise is trending, whatever its
    // relative bandwidth happens to be doing.
    CHECK(r.trend_strength > 1.0);                   // EMA spread, in ATR units
    CHECK(r.regime == VenueRegime::trend_up);
}

TEST_CASE("reference engine marks WARMING with insufficient history") {
    auto w = ramp(30, 100.0, 0.01);
    const auto r = compute_reference(w, LiteContext{});
    CHECK(r.warming);
    CHECK_FALSE(valid(r, RF_EMA_200));   // not enough bars for EMA200
    CHECK(valid(r, RF_RSI_14));          // but RSI(14) is available
}

TEST_CASE("reference engine projects AltSpotter aggregates without inventing data") {
    auto w = ramp(60, 100.0, 0.0); // flat
    LiteContext ctx;
    ctx.buy_volume = 70.0;
    ctx.sell_volume = 30.0;
    ctx.signed_volume_base = 40.0;
    ctx.spread = 0.5;
    ctx.relative_spread = 0.0005;
    ctx.bid_depth5 = 10.0;
    ctx.ask_depth5 = 12.0;
    ctx.book_imbalance = -0.1;
    ctx.mark_price = 100.5;
    ctx.index_price = 100.4;
    ctx.funding_rate = 0.0001;
    ctx.oi_change_ratio = 0.02;
    ctx.data_quality = 0.5;
    const double close = w.back().close;
    const auto r = compute_reference(w, ctx);

    REQUIRE(valid(r, RF_TRADE_IMBALANCE));
    CHECK_THAT(r.values[RF_TRADE_IMBALANCE], WithinAbs(0.4, 1e-5)); // (70-30)/100
    REQUIRE(valid(r, RF_SIGNED_VOLUME));
    CHECK_THAT(r.values[RF_SIGNED_VOLUME], WithinAbs(40.0 * close, 1e-2));
    REQUIRE(valid(r, RF_MARK_DEVIATION));
    CHECK(r.values[RF_MARK_DEVIATION] > 0.0);  // mark above close
    // Funding is unavailable until the settlement interval has been measured.
    CHECK_FALSE(valid(r, RF_FUNDING_8H));
    REQUIRE(valid(r, RF_DATA_QUALITY));
    CHECK_THAT(r.values[RF_DATA_QUALITY], WithinAbs(0.5, 1e-9));
    // A field the venue did not deliver stays invalid, never a fabricated zero.
    CHECK_FALSE(valid(r, RF_VWAP_RELATION));
}

TEST_CASE("low data quality forces LOW_LIQUIDITY regime") {
    auto w = ramp(220, 100.0, 0.05);
    LiteContext ctx;
    ctx.data_quality = 0.0;
    const auto r = compute_reference(w, ctx);
    CHECK(r.regime == VenueRegime::low_liquidity);
}

TEST_CASE("swing-zone detection bands the nearer extreme") {
    auto w = ramp(60, 100.0, 0.02); // rising -> recent high is resistance-side near close
    const auto z = detect_swing_zone(w);
    REQUIRE(z.has_value());
    CHECK(z->lower <= z->center);
    CHECK(z->center <= z->upper);
    CHECK(z->upper > z->lower);
}

TEST_CASE("funding is normalized to 8 hours once the interval is known") {
    auto w = ramp(60, 100.0, 0.0);
    LiteContext ctx;
    ctx.funding_rate = 0.0001;      // quoted per settlement
    ctx.funding_interval_hours = 4.0; // settled every 4 hours
    const auto r = compute_reference(w, ctx);
    REQUIRE(valid(r, RF_FUNDING_8H));
    CHECK_THAT(r.values[RF_FUNDING_8H], WithinAbs(0.0002, 1e-9)); // twice the 4h rate
}

TEST_CASE("cumulative volume delta accumulates the taker split over the window") {
    auto w = ramp(60, 100.0, 0.0);
    for (auto& c : w) {
        c.taker_buy_base_volume = 6.0;
        c.taker_sell_base_volume = 4.0;
    }
    const auto r = compute_reference(w, LiteContext{});
    REQUIRE(valid(r, RF_CVD));
    REQUIRE(valid(r, RF_SIGNED_VOLUME));
    // CVD is the whole window; signed volume is only the last bar.
    CHECK(r.values[RF_CVD] > r.values[RF_SIGNED_VOLUME] * 50.0F);
}

TEST_CASE("RSI matches Wilder smoothing on a monotone series") {
    // An unbroken sequence of gains has zero average loss, so Wilder RSI is 100.
    auto w = ramp(60, 100.0, 0.5);
    const auto r = compute_reference(w, LiteContext{});
    REQUIRE(valid(r, RF_RSI_14));
    CHECK_THAT(r.values[RF_RSI_14], WithinAbs(100.0, 1e-3));
}

TEST_CASE("the volatility axis is an empirical percentile, not an absolute level") {
    // Two series that differ only by a price scale factor must produce the same
    // volatility percentile: the axis may not depend on the price level.
    auto small = ramp(220, 10.0, 0.005);
    auto large = ramp(220, 10000.0, 5.0);
    const auto rs = compute_reference(small, LiteContext{});
    const auto rl = compute_reference(large, LiteContext{});
    REQUIRE(valid(rs, RF_BB_WIDTH_PERCENTILE));
    REQUIRE(valid(rl, RF_BB_WIDTH_PERCENTILE));
    CHECK_THAT(rs.values[RF_BB_WIDTH_PERCENTILE],
               WithinAbs(rl.values[RF_BB_WIDTH_PERCENTILE], 1e-6));
}

TEST_CASE("a flat market with no direction is a range, not a trend") {
    auto w = ramp(220, 100.0, 0.0);
    LiteContext ctx;
    ctx.data_quality = 1.0;
    const auto r = compute_reference(w, ctx);
    CHECK(r.regime != VenueRegime::trend_up);
    CHECK(r.regime != VenueRegime::trend_down);
}

// --- reference engine vs the real TA-Lib ------------------------------------
//
// `reference_engine.cpp` computes RSI and ATR by hand, with a comment asserting
// they match TA-Lib's Wilder recursion. That is a SECOND independent
// mathematical implementation of a function the project already links, and an
// assertion in a comment is not a verification. These cases run the real
// library on the same input and require agreement.
//
// The reference venues (Binance, Bybit, OKX) carry only this hand-written path,
// so if it drifts from TA-Lib, three of the four venues silently disagree with
// the fourth about what "RSI(14)" means.

TEST_CASE("reference RSI(14) matches TA-Lib RSI on identical input", "[venue_lite][talib]") {
    // A ramp is monotone, which pins RSI at 100 and proves nothing. Use a
    // series that actually turns.
    std::vector<LiteCandle> w;
    double p = 100.0;
    for (std::size_t i = 0; i < 200; ++i) {
        const double step = std::sin(static_cast<double>(i) / 7.0) * 1.5
                          + std::sin(static_cast<double>(i) / 3.0) * 0.4;
        LiteCandle c;
        c.open_time_ms = static_cast<int64_t>(i) * 60'000;
        c.open = p;
        c.close = p + step;
        c.high = std::max(c.open, c.close) + 0.3;
        c.low = std::min(c.open, c.close) - 0.3;
        c.base_volume = 100.0;
        c.quote_volume = 100.0 * p;
        p = c.close;
        w.push_back(c);
    }

    std::vector<double> close, high, low;
    for (const auto& c : w) { close.push_back(c.close); high.push_back(c.high); low.push_back(c.low); }

    REQUIRE(TA_Initialize() == TA_SUCCESS);

    // TA-Lib RSI(14) at the last bar.
    int out_beg = 0, out_n = 0;
    std::vector<double> ta_rsi(close.size(), 0.0);
    REQUIRE(TA_RSI(0, static_cast<int>(close.size()) - 1, close.data(), 14,
                   &out_beg, &out_n, ta_rsi.data()) == TA_SUCCESS);
    REQUIRE(out_n > 0);
    const double talib_rsi_last = ta_rsi[static_cast<std::size_t>(out_n) - 1];

    // TA-Lib ATR(14) at the last bar.
    int atr_beg = 0, atr_n = 0;
    std::vector<double> ta_atr(close.size(), 0.0);
    REQUIRE(TA_ATR(0, static_cast<int>(close.size()) - 1, high.data(), low.data(), close.data(),
                   14, &atr_beg, &atr_n, ta_atr.data()) == TA_SUCCESS);
    REQUIRE(atr_n > 0);
    const double talib_atr_last = ta_atr[static_cast<std::size_t>(atr_n) - 1];

    LiteContext ctx{};
    const auto ref = compute_reference(w, ctx);

    // Same quantity, computed twice by two implementations. They agree to the
    // precision the storage allows and no further: `values` is a float array,
    // so ~7 significant digits is the whole budget. A relative tolerance of
    // 1e-6 is therefore the tightest honest bound — measured agreement at this
    // tolerance was RSI 82.74996 vs 82.7499594296, ATR 1.50302 vs 1.5030161511.
    // Anything looser would stop catching a real divergence in Wilder's
    // recursion; anything tighter tests float rounding, not the mathematics.
    CHECK_THAT(static_cast<double>(ref.values[RF_RSI_14]),
               WithinRel(talib_rsi_last, 1e-6));
    CHECK_THAT(static_cast<double>(ref.values[RF_ATR_14]),
               WithinRel(talib_atr_last, 1e-6));

    TA_Shutdown();
}

// BBO dynamics. market_bbo_1m summarises the continuous top-of-book stream —
// measured on VPS1 at 206-909 updates per minute against ~7 order-book
// snapshots — so it is the only record of what the spread did INSIDE the bar.
// It was stored and read by nothing before this feature block existed.
TEST_CASE("reference engine derives BBO dynamics without inventing readings") {
    const auto w = ramp(260, 100.0, 0.05);

    SECTION("a normal minute yields spreads in bps and a bounded stability") {
        LiteContext ctx;
        ctx.bbo_bid_first = 99.95;
        ctx.bbo_ask_first = 100.05;   // mid 100.00
        ctx.bbo_bid_last = 100.00;
        ctx.bbo_ask_last = 100.10;    // mid 100.05
        ctx.bbo_spread_min = 0.05;    // 5 bps of a ~100 mid
        ctx.bbo_spread_max = 0.15;    // 15 bps
        ctx.bbo_sample_count = 540;

        const auto r = compute_reference(w, ctx);
        REQUIRE(valid(r, RF_BBO_SPREAD_MIN_BPS));
        REQUIRE(valid(r, RF_BBO_SPREAD_MAX_BPS));
        CHECK_THAT(r.values[RF_BBO_SPREAD_MIN_BPS], WithinRel(5.0, 0.01));
        CHECK_THAT(r.values[RF_BBO_SPREAD_MAX_BPS], WithinRel(15.0, 0.01));
        CHECK_THAT(r.values[RF_BBO_SPREAD_RANGE_BPS], WithinRel(10.0, 0.01));
        CHECK_THAT(r.values[RF_BBO_SPREAD_MEAN_BPS], WithinRel(10.0, 0.01));
        // range == mean here, so the quote was maximally unstable: 1 - 10/10 = 0.
        CHECK_THAT(r.values[RF_BBO_STABILITY], WithinAbs(0.0, 1e-6));
        CHECK(r.values[RF_BBO_STABILITY] >= 0.0);
        CHECK(r.values[RF_BBO_STABILITY] <= 1.0);
        CHECK_THAT(r.values[RF_BBO_SAMPLE_RATE], WithinAbs(540.0, 1e-6));
    }

    SECTION("a steady quote scores near 1") {
        LiteContext ctx;
        ctx.bbo_bid_first = 99.95;
        ctx.bbo_ask_first = 100.05;
        ctx.bbo_bid_last = 99.95;
        ctx.bbo_ask_last = 100.05;
        ctx.bbo_spread_min = 0.100;
        ctx.bbo_spread_max = 0.101;   // barely moved
        ctx.bbo_sample_count = 800;

        const auto r = compute_reference(w, ctx);
        REQUIRE(valid(r, RF_BBO_STABILITY));
        CHECK(r.values[RF_BBO_STABILITY] > 0.98);
        CHECK_THAT(r.values[RF_BBO_MID_DRIFT], WithinAbs(0.0, 1e-9));
    }

    SECTION("absent BBO leaves every derived feature unset, not zero") {
        const auto r = compute_reference(w, LiteContext{});
        CHECK_FALSE(valid(r, RF_BBO_SPREAD_MIN_BPS));
        CHECK_FALSE(valid(r, RF_BBO_SPREAD_MAX_BPS));
        CHECK_FALSE(valid(r, RF_BBO_SPREAD_RANGE_BPS));
        CHECK_FALSE(valid(r, RF_BBO_SPREAD_MEAN_BPS));
        CHECK_FALSE(valid(r, RF_BBO_STABILITY));
        CHECK_FALSE(valid(r, RF_BBO_MID_DRIFT));
        CHECK_FALSE(valid(r, RF_BBO_SAMPLE_RATE));
    }

    SECTION("an inconsistent venue reading is skipped, not published negative") {
        LiteContext ctx;
        ctx.bbo_bid_first = 99.95;
        ctx.bbo_ask_first = 100.05;
        ctx.bbo_bid_last = 99.95;
        ctx.bbo_ask_last = 100.05;
        ctx.bbo_spread_min = 0.20;    // min > max: impossible
        ctx.bbo_spread_max = 0.10;
        ctx.bbo_sample_count = 100;

        const auto r = compute_reference(w, ctx);
        CHECK_FALSE(valid(r, RF_BBO_SPREAD_RANGE_BPS));
        // Sample count stands on its own and is still reported.
        CHECK(valid(r, RF_BBO_SAMPLE_RATE));
    }

    SECTION("mid drift measures the quote, not the candle close") {
        LiteContext ctx;
        ctx.bbo_bid_first = 100.0;
        ctx.bbo_ask_first = 100.0;
        ctx.bbo_bid_last = 101.0;
        ctx.bbo_ask_last = 101.0;
        ctx.bbo_spread_min = 0.0;
        ctx.bbo_spread_max = 0.0;
        ctx.bbo_sample_count = 10;

        const auto r = compute_reference(w, ctx);
        REQUIRE(valid(r, RF_BBO_MID_DRIFT));
        CHECK_THAT(r.values[RF_BBO_MID_DRIFT], WithinRel(0.01, 1e-6));
    }
}

// The primary venue gets the BBO block and nothing else: its indicator ladder
// comes from the TA-Lib engine, which cannot see market_bbo_1m because it works
// from candles. Computing the ladder twice in two modules is exactly what this
// split avoids.
TEST_CASE("primary-venue BBO product carries only the BBO block") {
    LiteContext ctx;
    ctx.bbo_bid_first = 99.95;
    ctx.bbo_ask_first = 100.05;
    ctx.bbo_bid_last = 100.00;
    ctx.bbo_ask_last = 100.06;
    ctx.bbo_spread_min = 0.06;
    ctx.bbo_spread_max = 0.10;
    ctx.bbo_sample_count = 562;
    ctx.data_quality = 1.0;

    const auto r = compute_bbo_only(ctx);

    SECTION("BBO features are populated") {
        CHECK(valid(r, RF_BBO_SPREAD_MIN_BPS));
        CHECK(valid(r, RF_BBO_SPREAD_MAX_BPS));
        CHECK(valid(r, RF_BBO_SPREAD_RANGE_BPS));
        CHECK(valid(r, RF_BBO_STABILITY));
        CHECK(valid(r, RF_BBO_MID_DRIFT));
        CHECK_THAT(r.values[RF_BBO_SAMPLE_RATE], WithinAbs(562.0, 1e-6));
    }

    SECTION("derivatives reach the primary venue too") {
        // These come from market_derivatives_context_1m, which the TA-Lib
        // engine cannot see. Before this, the execution venue had no basis,
        // no funding and no OI change at all.
        LiteContext d;
        d.mid_price = 100.0;
        d.mark_price = 100.05;
        d.index_price = 99.95;
        d.funding_rate = 0.0001;
        d.funding_interval_hours = 8.0;
        d.oi_change_ratio = 0.02;
        const auto rd = compute_bbo_only(d);
        CHECK(valid(rd, RF_MARK_DEVIATION));
        CHECK(valid(rd, RF_INDEX_DEVIATION));
        CHECK(valid(rd, RF_FUNDING_8H));
        CHECK(valid(rd, RF_OI_CHANGE));
        CHECK_THAT(rd.values[RF_OI_CHANGE], WithinRel(0.02, 1e-6));
    }

    SECTION("no indicator is duplicated from the TA-Lib engine") {
        CHECK_FALSE(valid(r, RF_RSI_14));
        CHECK_FALSE(valid(r, RF_ATR_14));
        CHECK_FALSE(valid(r, RF_EMA_200));
        CHECK_FALSE(valid(r, RF_RETURN));
        CHECK_FALSE(valid(r, RF_BB_WIDTH_20));
        CHECK_FALSE(valid(r, RF_VOLUME_ZSCORE_20));
    }

    SECTION("it needs no candle history, so it is never WARMING") {
        CHECK_FALSE(r.warming);
    }

    SECTION("data quality is still reported") {
        CHECK(valid(r, RF_DATA_QUALITY));
    }
}

// Intra-minute flow and liquidity, from the 1s aggregate series. The
// microstructure module persists only 15s/30s/1m windows from the live stream,
// so nothing could see inside a 15-second window and nothing could see it at
// all historically. These summarise the SHAPE of the minute: a burst and steady
// flow with identical totals must not look the same.
TEST_CASE("intra-minute flow features distinguish a burst from steady flow") {
    const auto w = ramp(60, 100.0, 0.02);

    SECTION("a burst scores high on ratio and low on occupancy") {
        LiteContext ctx;
        ctx.flow_peak_volume_1s = 900.0;   // one second carried almost everything
        ctx.flow_mean_volume_1s = 20.0;
        ctx.flow_peak_trades_1s = 140.0;
        ctx.flow_active_seconds = 5.0 / 60.0;
        ctx.flow_signed_stddev = 180.0;

        const auto r = compute_reference(w, ctx);
        REQUIRE(valid(r, RF_FLOW_BURST_RATIO));
        CHECK_THAT(r.values[RF_FLOW_BURST_RATIO], WithinRel(45.0, 0.01));
        CHECK(r.values[RF_FLOW_ACTIVE_SECONDS] < 0.1);
        CHECK_THAT(r.values[RF_FLOW_PEAK_TRADES_1S], WithinAbs(140.0, 1e-6));
    }

    SECTION("steady flow scores near 1 with high occupancy") {
        LiteContext ctx;
        ctx.flow_peak_volume_1s = 22.0;
        ctx.flow_mean_volume_1s = 20.0;
        ctx.flow_active_seconds = 58.0 / 60.0;

        const auto r = compute_reference(w, ctx);
        REQUIRE(valid(r, RF_FLOW_BURST_RATIO));
        CHECK(r.values[RF_FLOW_BURST_RATIO] < 1.2);
        CHECK(r.values[RF_FLOW_ACTIVE_SECONDS] > 0.95);
    }

    SECTION("depth drop measures withdrawal, not thinness") {
        LiteContext thin;              // shallow all minute, never withdrew
        thin.book_depth_min_1s = 1000.0;
        thin.book_depth_mean_1s = 1000.0;
        const auto r_thin = compute_reference(w, thin);
        REQUIRE(valid(r_thin, RF_BOOK_DEPTH_DROP));
        CHECK_THAT(r_thin.values[RF_BOOK_DEPTH_DROP], WithinAbs(0.0, 1e-6));

        LiteContext pulled;            // deep on average, briefly emptied
        pulled.book_depth_min_1s = 200.0;
        pulled.book_depth_mean_1s = 1000.0;
        const auto r_pulled = compute_reference(w, pulled);
        CHECK_THAT(r_pulled.values[RF_BOOK_DEPTH_DROP], WithinRel(0.8, 0.01));
    }

    SECTION("absent 1s data leaves the block unset, not zero") {
        const auto r = compute_reference(w, LiteContext{});
        CHECK_FALSE(valid(r, RF_FLOW_PEAK_TRADES_1S));
        CHECK_FALSE(valid(r, RF_FLOW_BURST_RATIO));
        CHECK_FALSE(valid(r, RF_FLOW_ACTIVE_SECONDS));
        CHECK_FALSE(valid(r, RF_BOOK_DEPTH_MIN));
        CHECK_FALSE(valid(r, RF_BOOK_DEPTH_DROP));
    }

    SECTION("bounded features stay in range") {
        LiteContext ctx;
        ctx.flow_active_seconds = 2.0;     // impossible input
        ctx.book_depth_min_1s = 5000.0;    // min > mean: inconsistent
        ctx.book_depth_mean_1s = 1000.0;
        const auto r = compute_reference(w, ctx);
        CHECK(r.values[RF_FLOW_ACTIVE_SECONDS] <= 1.0);
        CHECK(r.values[RF_BOOK_DEPTH_DROP] >= 0.0);
    }
}

// Relative open-interest change must come from whichever column the venue
// actually populates, and both ends of the ratio must come from the SAME
// column.
//
// The regression: only `open_interest_quote` was read. Measured over 30 minutes
// on VPS1, quote covered 100% of Bybit rows and 58% of OKX but 0% of Bitget and
// Binance, while `open_interest_base` covered 96-100% everywhere. RF_OI_CHANGE
// was therefore invalid in 833 of 833 vectors on the execution venue, and the
// cross-venue OI "consensus" was built from two venues out of four.
TEST_CASE("open-interest change uses a consistent unit and is never mixed") {
    const auto w = ramp(260, 100.0, 0.05);

    SECTION("a venue reporting only base units still produces the feature") {
        LiteContext ctx;
        ctx.oi_change_ratio = (1100.0 - 1000.0) / 1000.0;   // as venue_store now derives it
        const auto r = compute_reference(w, ctx);
        REQUIRE(valid(r, RF_OI_CHANGE));
        CHECK_THAT(r.values[RF_OI_CHANGE], WithinRel(0.10, 1e-6));  // stored as float32
    }

    SECTION("the ratio is unit-invariant when both ends share a column") {
        // Same 10% growth expressed in coins and in USDT must give one number.
        const double in_base = (1100.0 - 1000.0) / 1000.0;
        const double in_quote = (1100.0 * 77.0 - 1000.0 * 77.0) / (1000.0 * 77.0);
        CHECK_THAT(in_base, WithinRel(in_quote, 1e-12));
    }

    SECTION("absent OI leaves the feature unset, never zero") {
        const auto r = compute_reference(w, LiteContext{});
        CHECK_FALSE(valid(r, RF_OI_CHANGE));
    }
}
