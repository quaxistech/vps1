// Regressions for the macro block. Five properties, each of which would be
// invisible in production and expensive in a model.

#include "macro_availability.hpp"
#include "macro_compute.hpp"
#include "macro_features.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace alttech::modules::macro;

namespace {

constexpr int64_t kMinute = 60'000;
constexpr int64_t kDay = 86'400'000;
constexpr int64_t kMon = 1767571200000LL;   // 2026-01-05T00:00:00Z, a Monday

IntradayPoint pt(int64_t t, double mid, bool traded = true, double spread = 1.0) {
    IntradayPoint p;
    p.t_ms = t; p.mid = mid; p.spread_bps = spread; p.traded = traded;
    return p;
}

// The newest 1m bar the feed could have delivered by t: intraday inputs in
// these tests are placed there, because a bar stamped t itself is, by the
// hourly-file rule, not yet anybody's number at t.
int64_t tick(int64_t t) { return intraday_cutoff_ms(t) - kMinute; }

size_t pos_of(const std::string& name) {
    const auto& names = macro_feature_names();
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) return i;
    }
    FAIL("no such macro feature: " + name);
    return 0;
}

} // namespace

TEST_CASE("the catalog and the computation agree on width", "[macro][schema]") {
    // The one mismatch that cannot be caught by reading: a feature added to the
    // catalog but never written leaves every later column shifted by one, and
    // every value downstream would then be attributed to the wrong name.
    MacroInputs in;
    in.usdcnh.push_back(pt(tick(kMon), 7.0));
    const auto v = compute_macro_series(in, kMon, kMon + kMinute);
    REQUIRE(v.size() == 1);
    CHECK(v[0].values.size() == macro_feature_count());
    CHECK(v[0].validity.size() == (macro_feature_count() + 7) / 8);
    // And the fingerprint is what a stored feature_set_version is checked
    // against, so it must be stable and non-trivial.
    CHECK(macro_schema_fingerprint().size() == 64);
}

TEST_CASE("the three CNY observations stay separate series", "[macro][cny]") {
    // USDCNH is a traded offshore price, the PBOC parity is a policy setting and
    // H.10 is a reference observation. On a real date they read 6.7096, 6.7766
    // and 6.7108. The block must publish the DIFFERENCES; collapsing them into
    // one "CNY" column would destroy the only interesting thing about them.
    MacroInputs in;
    in.usdcnh.push_back(pt(tick(kMon), 6.7096));
    in.fixing.push_back(DailyPoint{kMon - kDay, 6.7766});
    in.h10_cny.push_back(DailyPoint{kMon - 10 * kDay, 6.7108});

    const auto v = compute_macro_series(in, kMon, kMon + kMinute);
    REQUIRE(v.size() == 1);

    const size_t gap_fix = pos_of("cnh_vs_fixing_bps");
    const size_t gap_h10 = pos_of("cnh_vs_h10_bps");
    const size_t fix_h10 = pos_of("fixing_vs_h10_bps");
    REQUIRE(v[0].is_valid(gap_fix));
    REQUIRE(v[0].is_valid(gap_h10));
    REQUIRE(v[0].is_valid(fix_h10));

    // Market below the fix: the offshore renminbi is stronger than policy says.
    CHECK(v[0].values[gap_fix] < 0.0f);
    // All three differ from each other, which is the point.
    CHECK(v[0].values[gap_fix] != v[0].values[gap_h10]);
    CHECK(std::abs(v[0].values[fix_h10]) > 1.0f);
}

TEST_CASE("a daily figure is unusable until it is published", "[macro][asof]") {
    // The look-ahead that would be easiest to ship and hardest to see. The Fed's
    // H.10 covering a Monday is not public that Monday -- it is a weekly release
    // the following week. Reading it on the observation date hands a model a
    // number nobody had.
    MacroInputs in;
    in.usdcnh.push_back(pt(tick(kMon), 7.0));
    in.h10_cny.push_back(DailyPoint{kMon, 6.9});   // observed today

    const auto same_day = compute_macro_series(in, kMon, kMon + kMinute);
    REQUIRE(same_day.size() == 1);
    CHECK_FALSE(same_day[0].is_valid(pos_of("cnh_vs_h10_bps")));

    // The release is the Monday after, in New York; the block admits it from
    // the Tuesday. A minute before that it is still nobody's number.
    const int64_t usable = kMon + 8 * kDay;
    MacroInputs later = in;
    later.usdcnh.clear();
    later.usdcnh.push_back(pt(tick(usable - kMinute), 7.0));
    const auto before = compute_macro_series(later, usable - kMinute, usable);
    REQUIRE(before.size() == 1);
    CHECK_FALSE(before[0].is_valid(pos_of("cnh_vs_h10_bps")));
    later.usdcnh.clear();
    later.usdcnh.push_back(pt(tick(usable), 7.0));
    const auto after = compute_macro_series(later, usable, usable + kMinute);
    REQUIRE(after.size() == 1);
    CHECK(after[0].is_valid(pos_of("cnh_vs_h10_bps")));

    // The schedule is the aggregator's, on the observation's weekday. FRED
    // carries a Treasury or index close the NEXT business day, so a Thursday
    // close is admitted on Saturday and a Friday close not before Tuesday --
    // the same rule that admits a Monday H.10 figure eight days on and a
    // Friday one four days on.
    constexpr int64_t kThu = kMon + 3 * kDay, kFri = kMon + 4 * kDay;
    CHECK(available_at_ms("RATE.US.10Y", kThu) == kThu + 2 * kDay);
    CHECK(available_at_ms("RATE.US.10Y", kFri) == kFri + 4 * kDay);
    CHECK(available_at_ms("IDX.VIX", kMon) == kMon + 2 * kDay);
    CHECK(available_at_ms("REF.USDCNY.FED", kMon) == kMon + 8 * kDay);
    CHECK(available_at_ms("REF.USDCNY.FED", kFri) == kFri + 4 * kDay);
    // A series this block does not know is admitted no sooner than its slowest.
    CHECK(available_at_ms("REF.NEW.SERIES", kMon) >= kMon + 8 * kDay);

    // The intraday feed is hourly files. A bar stamped 14:20 is not here at
    // 14:21 -- its hour has not ended -- and the block must not read it until
    // the file has had time to land: from 15:30. A vector at 15:29 still sees
    // the previous hour.
    {
        constexpr int64_t kH = 3'600'000;
        const int64_t bar = kMon + 14 * kH + 20 * kMinute;
        MacroInputs live;
        live.usdcnh.push_back(pt(bar, 7.0));
        live.usdcnh.push_back(pt(bar - 5 * kMinute, 7.1));
        const auto early = compute_macro_series(live, bar + kMinute, bar + 2 * kMinute);
        REQUIRE(early.size() == 1);
        CHECK_FALSE(early[0].is_valid(pos_of("cnh_spread_bps")));
        const auto before = compute_macro_series(live, kMon + 15 * kH + 29 * kMinute,
                                                 kMon + 15 * kH + 30 * kMinute);
        CHECK_FALSE(before[0].is_valid(pos_of("cnh_spread_bps")));
        // At 15:30 the 14:00 hour has landed; the newest usable bar is 14:59,
        // absent here, so the block reads nothing rather than the 14:20 bar.
        // Give it 14:59 and the spread is usable.
        live.usdcnh.push_back(pt(kMon + 14 * kH + 59 * kMinute, 7.0));
        const auto after = compute_macro_series(live, kMon + 15 * kH + 30 * kMinute,
                                                kMon + 15 * kH + 31 * kMinute);
        CHECK(after[0].is_valid(pos_of("cnh_spread_bps")));
        CHECK(intraday_cutoff_ms(kMon + 15 * kH + 30 * kMinute) == kMon + 15 * kH);
        CHECK(intraday_cutoff_ms(kMon + 15 * kH + 29 * kMinute) == kMon + 14 * kH);
    }

    // The PBOC parity is the exception and is set the same morning, so it must
    // NOT be held back a week -- but it is admitted only once the aggregator
    // that carries it has had time to, not at the minute Beijing sets it.
    CHECK(available_at_ms("REF.USDCNY.PBOC", kMon) == kMon + 8 * 3'600'000);
    CHECK(available_at_ms("REF.USDCNY.PBOC", kMon) < available_at_ms("REF.USDCNY.FED", kMon));
}

TEST_CASE("a carried-forward quote yields no return", "[macro][stale]") {
    // Across a weekend the feed repeats Friday's price at zero volume. A return
    // computed over that stretch is exactly zero -- a number that looks like a
    // measured calm market and is an artefact of the carry-forward.
    MacroInputs in;
    in.usdcnh.push_back(pt(tick(kMon) - kMinute, 7.0, /*traded=*/false));
    in.usdcnh.push_back(pt(tick(kMon), 7.0, /*traded=*/false));
    in.usdcnh.push_back(pt(tick(kMon) - 5 * kMinute, 7.0, /*traded=*/false));

    const auto v = compute_macro_series(in, kMon, kMon + kMinute);
    REQUIRE(v.size() == 1);
    CHECK_FALSE(v[0].is_valid(pos_of("cnh_ret_5m")));
    // The spread is a property of the quote, not of a move, and is equally
    // unpublished when the quote was never real.
    CHECK_FALSE(v[0].is_valid(pos_of("cnh_spread_bps")));
}

TEST_CASE("absent inputs leave a clear bit, never a zero measurement",
          "[macro][missing]") {
    // Nothing is available at all. Every position must read as unusable rather
    // than as a market sitting at zero.
    MacroInputs in;
    in.usdcnh.push_back(pt(tick(kMon), 7.0));
    const auto v = compute_macro_series(in, kMon, kMon + kMinute);
    REQUIRE(v.size() == 1);

    for (const char* name : {"vix_level", "us10y_level", "curve_2s10s", "breakeven_10y",
                             "ndx_ret_1d", "usdkrw_ret_1d"}) {
        const size_t p = pos_of(name);
        CHECK_FALSE(v[0].is_valid(p));
        CHECK(v[0].values[p] == 0.0f);   // the placeholder, guarded by the bit
    }
    CHECK(v[0].status != VectorStatus::valid);
}

TEST_CASE("daily z-scores are primed from history, not from the minute loop",
          "[macro][zscore]") {
    // The defect: both rolling z-scores sat at zero validity for the entire
    // first deployment. They are daily-cadence statistics needing twenty
    // observations, and a live cycle that sees four hours of minutes plus two
    // days of warm-up can never accumulate that from the loop alone.
    MacroInputs in;
    // A month of calm VIX history before the window, all published, then a
    // spike on the Friday: the close FRED admits on the Tuesday the window
    // starts, so it enters through the minute loop and not the priming.
    constexpr int64_t kTue = kMon + kDay;
    for (int d = 30; d >= 4; --d) {
        in.vix.push_back(DailyPoint{kMon - d * kDay, 15.0 + (d % 5)});
    }
    in.vix.push_back(DailyPoint{kMon - 3 * kDay, 30.0});
    in.usdcnh.push_back(pt(tick(kTue), 7.0));

    const auto v = compute_macro_series(in, kTue, kTue + kMinute);
    REQUIRE(v.size() == 1);
    const size_t z = pos_of("vix_z_20d");
    REQUIRE(v[0].is_valid(z));
    CHECK(v[0].values[z] > 2.0f);   // a spike against a calm history reads as one

    // And the CNH fix-gap z-score primes from the daily mid series the same way.
    for (int d = 30; d >= 1; --d) {
        in.fixing.push_back(DailyPoint{kMon - d * kDay, 7.05});
        in.cnh_daily_mid.push_back(DailyPoint{kMon - d * kDay, 7.05 + 0.001 * (d % 3)});
    }
    const auto v2 = compute_macro_series(in, kTue, kTue + kMinute);
    CHECK(v2[0].is_valid(pos_of("cnh_vs_fixing_z_20d")));
}

TEST_CASE("a closed FX market publishes no realized volatility", "[macro][weekend]") {
    // The defect: the rolling volatility counted RETURNS, not minutes. With no
    // new returns over a weekend, Friday's last sixty stayed in the buffer and
    // `eurusd_realized_vol_1h` was published as valid all Saturday and Sunday
    // -- a measurement of a market that was not trading.
    constexpr int64_t kH = 3'600'000;
    const int64_t fri = kMon - 3 * kDay;             // 2026-01-02, a Friday
    MacroInputs in;
    // Friday 20:00-21:00 trades every minute; nothing after (the market shut).
    for (int m = 0; m <= 60; ++m) {
        in.eurusd.push_back(pt(fri + 20 * kH + m * kMinute, 1.10 + 0.0001 * (m % 7)));
    }
    in.usdcnh.push_back(pt(tick(fri + 22 * kH), 7.0));
    const size_t vol = pos_of("eurusd_realized_vol_1h");

    // While the hour is fresh it is a real measurement.
    const auto open = compute_macro_series(in, fri + 22 * kH, fri + 22 * kH + kMinute);
    REQUIRE(open.size() == 1);
    CHECK(open[0].is_valid(vol));

    // Saturday noon: the last traded minute is fifteen hours old.
    const int64_t sat_noon = fri + kDay + 12 * kH;
    in.usdcnh.push_back(pt(tick(sat_noon), 7.0, /*traded=*/false));
    const auto closed = compute_macro_series(in, sat_noon, sat_noon + kMinute);
    REQUIRE(closed.size() == 1);
    CHECK_FALSE(closed[0].is_valid(vol));
}

TEST_CASE("equity_age_hours is the age of the oldest equity input", "[macro][age]") {
    // The defect: the column was VIX's age alone, while spx_ret_1d and
    // ndx_ret_1d come from separate series whose newest usable close can be a
    // different day. Here SPX is a day behind VIX and NDX.
    const int64_t thu_noon = kMon + 3 * kDay + 12 * 3'600'000;
    MacroInputs in;
    in.usdcnh.push_back(pt(tick(thu_noon), 7.0));
    // Monday's closes are usable Wednesday 00:00, Tuesday's Thursday 00:00.
    in.vix = {DailyPoint{kMon, 15.0}, DailyPoint{kMon + kDay, 16.0}};
    in.ndx = {DailyPoint{kMon, 100.0}, DailyPoint{kMon + kDay, 101.0}};
    in.spx = {DailyPoint{kMon - kDay, 50.0}, DailyPoint{kMon, 51.0}};
    const auto v = compute_macro_series(in, thu_noon, thu_noon + kMinute);
    REQUIRE(v.size() == 1);
    const size_t age = pos_of("equity_age_hours");
    REQUIRE(v[0].is_valid(age));
    CHECK(v[0].values[age] == 36.0f);   // SPX: usable Wednesday 00:00, 36 h before

    // No equity input at all: absent, never a zero age.
    MacroInputs none;
    none.usdcnh.push_back(pt(tick(thu_noon), 7.0));
    const auto w = compute_macro_series(none, thu_noon, thu_noon + kMinute);
    CHECK_FALSE(w[0].is_valid(age));
}
