// The path is where a silent, total corruption is easiest: the month in
// Dukascopy's URL is ZERO-BASED, so an off-by-one fetches a real file for the
// wrong month and every check downstream passes.

#include "altmacro/providers/dukascopy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <ctime>

using altmacro::domain::PriceBasis;
using altmacro::providers::DukascopyProvider;

TEST_CASE("January is month 00", "[dukascopy][path]") {
    const int64_t jan_2 = 1767312000000LL;   // 2026-01-02T00:00:00Z
    CHECK(DukascopyProvider::bar_path("EURUSD", jan_2, PriceBasis::bid) ==
          "/datafeed/EURUSD/2026/00/02/BID_candles_min_1.bi5");
}

TEST_CASE("December is month 11, across the year boundary", "[dukascopy][path]") {
    const int64_t dec_31 = 1798675200000LL;  // 2026-12-31T00:00:00Z
    CHECK(DukascopyProvider::bar_path("XAUUSD", dec_31, PriceBasis::ask) ==
          "/datafeed/XAUUSD/2026/11/31/ASK_candles_min_1.bi5");

    const int64_t jan_1_next = 1798761600000LL;  // 2027-01-01T00:00:00Z
    CHECK(DukascopyProvider::bar_path("XAUUSD", jan_1_next, PriceBasis::bid) ==
          "/datafeed/XAUUSD/2027/00/01/BID_candles_min_1.bi5");
}

TEST_CASE("the month boundary advances the month, not the day", "[dukascopy][path]") {
    const int64_t sep_30 = 1790726400000LL;  // 2026-09-30T00:00:00Z
    const int64_t oct_1  = 1790812800000LL;  // 2026-10-01T00:00:00Z
    CHECK(DukascopyProvider::bar_path("USDCNH", sep_30, PriceBasis::bid) ==
          "/datafeed/USDCNH/2026/08/30/BID_candles_min_1.bi5");
    CHECK(DukascopyProvider::bar_path("USDCNH", oct_1, PriceBasis::bid) ==
          "/datafeed/USDCNH/2026/09/01/BID_candles_min_1.bi5");
}

TEST_CASE("bid and ask are different files", "[dukascopy][path]") {
    const int64_t day = 1788825600000LL;
    CHECK(DukascopyProvider::bar_path("USDCNH", day, PriceBasis::bid) !=
          DukascopyProvider::bar_path("USDCNH", day, PriceBasis::ask));
}

TEST_CASE("tick paths carry the hour of day", "[dukascopy][path]") {
    const int64_t ten_utc = 1788861600000LL;  // 2026-09-08T10:00:00Z
    CHECK(DukascopyProvider::tick_path("EURUSD", ten_utc) ==
          "/datafeed/EURUSD/2026/08/08/10h_ticks.bi5");
    const int64_t midnight = 1788825600000LL;
    CHECK(DukascopyProvider::tick_path("EURUSD", midnight) ==
          "/datafeed/EURUSD/2026/08/08/00h_ticks.bi5");
}

TEST_CASE("the discovery probe hour moves with the calendar", "[dukascopy][probe]") {
    // A frozen probe date works until it doesn't: it stays put while the
    // calendar moves, so an instrument listed after it would be probed on a day
    // it did not exist and reported absent for good.
    const int64_t friday_noon = 1789084800000LL + 43'200'000;   // 2026-09-11 12:00Z
    const int64_t probe = DukascopyProvider::probe_hour_ms(friday_noon);

    // Always a Tuesday at 10:00 UTC.
    const std::time_t secs = static_cast<std::time_t>(probe / 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    CHECK(tm.tm_wday == 2);
    CHECK(tm.tm_hour == 10);
    CHECK(tm.tm_min == 0);

    // At least a week old, so it is never inside the archive's write lag.
    CHECK(friday_noon - probe >= 7LL * 86'400'000);
    // And not absurdly old either; a fortnight is the worst case.
    CHECK(friday_noon - probe <= 14LL * 86'400'000);

    // It advances as time does.
    const int64_t a_month_later = friday_noon + 30LL * 86'400'000;
    CHECK(DukascopyProvider::probe_hour_ms(a_month_later) > probe);
}
