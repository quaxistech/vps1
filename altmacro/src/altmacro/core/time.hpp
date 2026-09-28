#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace altmacro::timeutil {

inline constexpr int64_t kMsPerMinute = 60'000;
inline constexpr int64_t kMsPerHour = 3'600'000;
inline constexpr int64_t kMsPerDay = 86'400'000;

// Current UTC wall clock in Unix milliseconds.
[[nodiscard]] int64_t now_ms();

// ISO-8601 UTC with millisecond precision: 2026-08-23T12:34:56.789Z
[[nodiscard]] std::string to_iso8601(int64_t ms);

// Parse ISO-8601 (subset produced by to_iso8601 / PostgreSQL) to Unix ms.
[[nodiscard]] std::optional<int64_t> parse_iso8601(std::string_view s);

// UTC hour-of-day [0,24) as a real number, used by the cyclic time features.
[[nodiscard]] constexpr double utc_hour_of_day(int64_t ms) noexcept {
    const int64_t in_day = ((ms % kMsPerDay) + kMsPerDay) % kMsPerDay;
    return static_cast<double>(in_day) / static_cast<double>(kMsPerHour);
}

} // namespace altmacro::timeutil
