#include "altmacro/cli/commands.hpp"
#include "altmacro/domain/calendar.hpp"
#include "altmacro/domain/validation.hpp"
#include "altmacro/providers/dukascopy.hpp"
#include "altmacro/providers/frankfurter.hpp"

#include <iostream>

namespace altmacro::cli {
namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

} // namespace

// Offline invariants that must hold before the binary is allowed near a
// provider. No network, no database: this is what a deployment check can run.
int run_selftest() {
    std::cout << "altmacro selftest\n";

    // The zero-based month is the single easiest way to silently fetch the
    // wrong month's data, so it is asserted rather than trusted.
    const int64_t jan_2_2026 = 1767312000000LL;  // 2026-01-02T00:00:00Z
    const auto p = providers::DukascopyProvider::bar_path("EURUSD", jan_2_2026,
                                                          domain::PriceBasis::bid);
    check(p == "/datafeed/EURUSD/2026/00/02/BID_candles_min_1.bi5",
          "dukascopy path uses a zero-based month (January is 00)");

    const int64_t dec_31_2026 = 1798675200000LL;  // 2026-12-31T00:00:00Z
    const auto p2 = providers::DukascopyProvider::bar_path("XAUUSD", dec_31_2026,
                                                           domain::PriceBasis::ask);
    check(p2 == "/datafeed/XAUUSD/2026/11/31/ASK_candles_min_1.bi5",
          "dukascopy path handles the year boundary (December is 11)");

    // A frankfurter symbol without an explicit source would silently query the
    // 97-source blend, which is nobody's published rate.
    check(!providers::FrankfurterProvider::parse_symbol("CNY").has_value(),
          "frankfurter refuses a symbol with no explicit provider");
    auto binding = providers::FrankfurterProvider::parse_symbol("CFETS:USD:CNY");
    check(binding.has_value() && binding->source == "CFETS" && binding->base == "USD" &&
              binding->quote == "CNY",
          "frankfurter parses PROVIDER:BASE:QUOTE");

    // The sanity band is the guard against a wrong decode scale.
    domain::SanityBand band{5.0, 9.0};
    std::vector<domain::PriceBar> bars;
    domain::PriceBar b;
    b.timeframe = "1m"; b.event_time_ms = 1'700'000'000'000;
    b.open = 67.0964; b.high = 67.0978; b.low = 67.0960; b.close = 67.0970;
    bars.push_back(b);
    auto report = domain::validate_bars(bars, band);
    check(report.rejected == 1 && bars.empty(),
          "a ten-times decode error is rejected by the sanity band");

    // A weekend must never be reported as a defect.
    const int64_t saturday = 1788566400000LL;  // 2026-09-05T00:00:00Z is a Saturday
    check(domain::expectation_at(domain::SessionCalendar::fx_24x5, saturday + 43'200'000) ==
              domain::Expectation::closed,
          "fx calendar reports Saturday midday as closed");
    const int64_t tuesday = 1788825600000LL;  // 2026-09-08T00:00:00Z is a Tuesday
    check(domain::expectation_at(domain::SessionCalendar::fx_24x5, tuesday + 43'200'000) ==
              domain::Expectation::open,
          "fx calendar reports a weekday midday as open");
    check(domain::expectation_at(domain::SessionCalendar::us_business, tuesday + 43'200'000) ==
              domain::Expectation::unknown,
          "business calendar declines to guess about a weekday it may not know is a holiday");

    std::cout << (failures == 0 ? "\nselftest passed\n" : "\nselftest FAILED\n");
    return failures == 0 ? 0 : 1;
}

} // namespace altmacro::cli
