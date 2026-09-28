#pragma once
// Internal UTC time API.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace alttech::timeutil {

inline constexpr int64_t kMsPerMinute = 60'000;

// Current UTC wall clock in Unix milliseconds.
[[nodiscard]] int64_t now_ms();

// Floor timestamp to the containing minute boundary.
[[nodiscard]] constexpr int64_t floor_minute(int64_t ms) noexcept {
    return ms - (ms % kMsPerMinute + kMsPerMinute) % kMsPerMinute;
}

// Floor to interval boundary in UTC (tf_minutes divides a day; anchored at epoch,
// which is midnight UTC, so day/hour boundaries align with UTC calendar).
[[nodiscard]] constexpr int64_t floor_interval(int64_t ms, int32_t tf_minutes) noexcept {
    const int64_t span = static_cast<int64_t>(tf_minutes) * kMsPerMinute;
    return ms - (ms % span + span) % span;
}

// ISO-8601 UTC with millisecond precision: 2026-07-09T12:34:56.789Z
[[nodiscard]] std::string to_iso8601(int64_t ms);

// Parse ISO-8601 (subset produced by to_iso8601 / PostgreSQL) to Unix ms.
[[nodiscard]] std::optional<int64_t> parse_iso8601(std::string_view s);

// Year-month key for monthly partitions, e.g. 202607.
[[nodiscard]] std::string month_key(int64_t ms);

} // namespace alttech::timeutil
