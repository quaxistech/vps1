#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace altspotter::integrity {

// A historical repair must not fetch/publish candles until the target month(s)
// are physically writable. Retention may have dropped older partitions under
// pressure; resurrecting them automatically would fight the storage budget.
// This is a READ-ONLY catalog query; do not replace it with ensure_partitions.
// It checks EVERY month in the inclusive range and works across year boundaries.
inline constexpr std::string_view kRepairPartitionsPresentSql = R"sql(
SELECT NOT EXISTS (
    SELECT 1 FROM generate_series(
        date_trunc('month', to_timestamp($1::bigint / 1000.0) AT TIME ZONE 'UTC'),
        date_trunc('month', to_timestamp($2::bigint / 1000.0) AT TIME ZONE 'UTC'),
        interval '1 month'
    ) AS needed(month_start)
    WHERE NOT EXISTS (
        SELECT 1 FROM altspotter_partition_bounds('market_candles_1m'::regclass) AS part
        WHERE part.lower_bound AT TIME ZONE 'UTC' <= needed.month_start
          AND part.upper_bound AT TIME ZONE 'UTC' >= needed.month_start + interval '1 month'
    )
)
)sql";

// The scheduling rules for repairing a history gap, kept pure so the defect
// they were extracted from stays covered by tests.
//
// A gap can be far longer than a single repair command may carry, so it is
// repaired over several dispatches. The original implementation always
// requested the window starting at `gap_start` and counted every dispatch as an
// attempt, which meant a gap longer than
// `max_attempts * max_gap_minutes_per_command` re-requested its own first chunk
// until the budget ran out and was then recorded as unrepairable — with most of
// its range never requested even once. In production that retired 510 gaps
// covering 3.18 million candles while 31 rows had actually been recovered.

inline constexpr int64_t kMsPerMinute = 60'000;

// The inclusive end of the command window that starts at `from_ms`.
[[nodiscard]] inline int64_t repair_window_end(int64_t from_ms, int64_t gap_end_ms,
                                               int64_t chunk_minutes) noexcept {
    const int64_t chunk_ms = chunk_minutes * kMsPerMinute;
    const int64_t bounded = from_ms + chunk_ms - kMsPerMinute;
    return bounded < gap_end_ms ? bounded : gap_end_ms;
}

// The attempt budget exists to retire ranges that cannot be recovered. A gap
// that recovered data since its last dispatch is not stuck, so its budget
// starts again for the window it has now reached.
[[nodiscard]] inline int next_attempt_number(bool cursor_advanced, int attempts) noexcept {
    return cursor_advanced ? 1 : attempts + 1;
}

// Backoff follows the same rule: progress resets it, being stuck grows it.
[[nodiscard]] inline int64_t backoff_ceiling_sec(bool cursor_advanced, int attempts,
                                                 int64_t base_sec, int64_t max_sec) noexcept {
    const int exponent = cursor_advanced ? 0 : (attempts < 10 ? attempts : 10);
    const int64_t grown = base_sec * (1LL << exponent);
    return grown < max_sec ? grown : max_sec;
}

// Attempts can now restart, so a gap id and an attempt number no longer
// identify a command. The window has to be part of the identity or JetStream
// deduplicates a legitimate new command against an old one.
[[nodiscard]] inline std::string repair_message_id(int64_t gap_id, int64_t from_ms,
                                                   int attempt) {
    return "repcmd:" + std::to_string(gap_id) + ":" + std::to_string(from_ms) + ":" +
           std::to_string(attempt);
}

} // namespace altspotter::integrity
