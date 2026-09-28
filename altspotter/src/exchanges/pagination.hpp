#pragma once

#include "core/error.hpp"

#include <cstdint>
#include <optional>

namespace altspotter::exchanges {

// Backwards history pagination contract: after fetching a page whose earliest
// candle open_time is `page_earliest_ms` for the exclusive window bound
// `cursor_end_ms`, the next window bound is `page_earliest_ms`.
//
// Errors when the cursor does not move (exchange keeps returning the same
// page); callers must stop instead of looping forever.
inline Result<int64_t> advance_history_cursor(int64_t page_earliest_ms, int64_t cursor_end_ms) {
    if (page_earliest_ms >= cursor_end_ms) {
        return fail(ErrorCode::upstream_error,
                    "history cursor did not advance (earliest " +
                        std::to_string(page_earliest_ms) + " >= cursor " +
                        std::to_string(cursor_end_ms) + ")");
    }
    return page_earliest_ms;
}

// True when the walk is complete: the page reached (or crossed) the target
// lower bound of the requested window.
inline bool history_walk_done(int64_t page_earliest_ms, int64_t target_from_ms) {
    return page_earliest_ms <= target_from_ms;
}

// Backwards pagination for funding-settlement feeds, which are served
// newest-first with a page cursor rather than a candle-style window.
//
// `oldest_ms` is the earliest settlement on the page just read and `cursor_ms`
// the bound that produced it (0 = unbounded first page). Returns the next
// cursor, or nullopt when the walk must stop because it would not advance --
// the venue returning the same page forever.
//
// `end_is_inclusive` distinguishes the two conventions in use: OKX's `after` is
// exclusive, so the next cursor is the oldest timestamp itself; Bybit's
// `endTime` is inclusive and must be stepped one millisecond past it.
//
// Without this walk a single page IS the whole history: 100 settlements is ~33
// days on OKX and 200 is ~66 days on Bybit, against a configured 180-day
// window. Both venues sat far short of Bitget's 90 days for exactly that
// reason.
inline std::optional<int64_t> advance_funding_cursor(int64_t oldest_ms, int64_t cursor_ms,
                                                     bool end_is_inclusive) {
    if (oldest_ms <= 0) return std::nullopt;
    if (cursor_ms > 0 && oldest_ms >= cursor_ms) return std::nullopt;
    return end_is_inclusive ? oldest_ms - 1 : oldest_ms;
}

} // namespace altspotter::exchanges
