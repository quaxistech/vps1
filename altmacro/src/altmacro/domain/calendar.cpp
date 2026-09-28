#include "altmacro/domain/calendar.hpp"

#include <ctime>

namespace altmacro::domain {
namespace {
constexpr int64_t kDayMs = 86'400'000;
constexpr int64_t kHourMs = 3'600'000;
// Daylight-saving moves the interbank open and close by an hour twice a year.
// Rather than encode two sets of rules, instants within this distance of a
// boundary are reported unknown.
constexpr int64_t kBoundarySlackMs = 90 * 60 * 1000;
} // namespace

const char* to_string(SessionCalendar c) noexcept {
    switch (c) {
        case SessionCalendar::none: return "none";
        case SessionCalendar::fx_24x5: return "fx_24x5";
        case SessionCalendar::us_business: return "us_business";
        case SessionCalendar::uk_business: return "uk_business";
        case SessionCalendar::cn_business: return "cn_business";
        case SessionCalendar::target2: return "target2";
        case SessionCalendar::us_equity: return "us_equity";
    }
    return "none";
}

SessionCalendar calendar_from_string(std::string_view s) noexcept {
    if (s == "fx_24x5") return SessionCalendar::fx_24x5;
    if (s == "us_business") return SessionCalendar::us_business;
    if (s == "uk_business") return SessionCalendar::uk_business;
    if (s == "cn_business") return SessionCalendar::cn_business;
    if (s == "target2") return SessionCalendar::target2;
    if (s == "us_equity") return SessionCalendar::us_equity;
    return SessionCalendar::none;
}

int utc_weekday(int64_t utc_ms) noexcept {
    const std::time_t secs = static_cast<std::time_t>(utc_ms / 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    return tm.tm_wday;
}

Expectation expectation_at(SessionCalendar cal, int64_t utc_ms) noexcept {
    if (cal == SessionCalendar::none) return Expectation::unknown;

    const int wday = utc_weekday(utc_ms);
    const int64_t in_day = ((utc_ms % kDayMs) + kDayMs) % kDayMs;

    if (cal == SessionCalendar::fx_24x5) {
        // Friday: open until 22:00 UTC, then shut for the week.
        if (wday == 5) {
            const int64_t close = 22 * kHourMs;
            if (in_day < close - kBoundarySlackMs) return Expectation::open;
            if (in_day > close + kBoundarySlackMs) return Expectation::closed;
            return Expectation::unknown;
        }
        // Saturday: shut all day, and far enough from either boundary to say so.
        if (wday == 6) return Expectation::closed;
        // Sunday: shut until 22:00 UTC, then the new week opens.
        if (wday == 0) {
            const int64_t open = 22 * kHourMs;
            if (in_day < open - kBoundarySlackMs) return Expectation::closed;
            if (in_day > open + kBoundarySlackMs) return Expectation::open;
            return Expectation::unknown;
        }
        return Expectation::open; // Monday through Thursday
    }

    // Business calendars: weekends are certainly shut; weekdays may be holidays
    // and this function has no holiday table, so it declines to guess.
    if (wday == 0 || wday == 6) return Expectation::closed;
    return Expectation::unknown;
}

} // namespace altmacro::domain
