#include "altmacro/domain/calendar.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace altmacro::domain;

namespace {
constexpr int64_t kMidday = 43'200'000;
constexpr int64_t kSaturday = 1788566400000LL;  // 2026-09-05T00:00:00Z
constexpr int64_t kTuesday  = 1788825600000LL;  // 2026-09-08T00:00:00Z
} // namespace

TEST_CASE("weekends are closed, not missing", "[calendar]") {
    CHECK(expectation_at(SessionCalendar::fx_24x5, kSaturday + kMidday) == Expectation::closed);
}

TEST_CASE("weekdays are open for FX", "[calendar]") {
    CHECK(expectation_at(SessionCalendar::fx_24x5, kTuesday + kMidday) == Expectation::open);
}

TEST_CASE("the business calendar declines to guess about weekdays", "[calendar]") {
    // A weekday can still be a holiday, and there is no holiday table here.
    // Reporting "unknown" is what keeps a public holiday from being filed as a
    // data defect.
    CHECK(expectation_at(SessionCalendar::us_business, kTuesday + kMidday) == Expectation::unknown);
    CHECK(expectation_at(SessionCalendar::us_business, kSaturday + kMidday) == Expectation::closed);
}

TEST_CASE("the FX week boundary is reported unknown rather than guessed", "[calendar]") {
    // Daylight saving moves the interbank open and close by an hour twice a
    // year; an instant inside that ambiguity is not asserted either way.
    const int64_t friday = kTuesday + 3 * 86'400'000;   // 2026-09-11
    CHECK(expectation_at(SessionCalendar::fx_24x5, friday + 22 * 3'600'000) ==
          Expectation::unknown);
    CHECK(expectation_at(SessionCalendar::fx_24x5, friday + 12 * 3'600'000) == Expectation::open);
}

TEST_CASE("'none' never asserts anything", "[calendar]") {
    CHECK(expectation_at(SessionCalendar::none, kSaturday + kMidday) == Expectation::unknown);
}
