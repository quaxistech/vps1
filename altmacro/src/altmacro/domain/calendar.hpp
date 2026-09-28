#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace altmacro::domain {

// Session calendars, used ONLY to annotate an absence — never to manufacture
// an expectation the source did not express.
//
// The temptation here is to hard-code a holiday calendar per market and call
// any weekday hole a defect. That would be wrong in both directions: we cannot
// know every exchange holiday, and a market can be shut for reasons no
// calendar lists. So expectation is taken from the source itself:
//
//   * Dukascopy publishes one file per day. The file exists or answers 404,
//     and inside it a minute with no quotes arrives as all-zero. The feed
//     therefore states which minutes were closed; we record what it said.
//   * FRED and the other daily sources emit one row per date they consider
//     valid, marking absent values with ".". The row set IS the calendar.
//
// What remains for this enum is the one thing a calendar can say honestly
// without a holiday table: whether an instant falls in a weekend, which is
// enough to distinguish a routine closure from a hole worth investigating.
enum class SessionCalendar {
    none,
    fx_24x5,
    us_business,
    uk_business,
    cn_business,
    target2,
    us_equity,
};

[[nodiscard]] const char* to_string(SessionCalendar) noexcept;
[[nodiscard]] SessionCalendar calendar_from_string(std::string_view) noexcept;

// Tri-state, because "I do not know" is a real and frequent answer here and
// collapsing it into yes or no is how a holiday becomes a false defect.
enum class Expectation { open, closed, unknown };

// Weekend-level judgement only.
//
// For fx_24x5 the week is taken as opening Sunday 22:00 UTC and closing Friday
// 22:00 UTC. That is the conventional interbank week and it matches what the
// Dukascopy files show, but it shifts by an hour across daylight-saving
// changes, so an instant within an hour of either boundary is reported
// `unknown` rather than guessed at.
//
// For the business calendars this returns `closed` on Saturday and Sunday and
// `unknown` on weekdays: a weekday may still be a holiday, and this function
// has no way to know which.
[[nodiscard]] Expectation expectation_at(SessionCalendar cal, int64_t utc_ms) noexcept;

// Convenience for reporting: 0 = Sunday .. 6 = Saturday, UTC.
[[nodiscard]] int utc_weekday(int64_t utc_ms) noexcept;

} // namespace altmacro::domain
