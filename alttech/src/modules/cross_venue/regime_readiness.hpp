#pragma once

// Whether the cross-venue sweep should leave a fresh bar for its next pass.
//
// The global regime of a bar is a vote over the four venues' 1m regimes, which
// venue_lite writes on its own sweep. Computed before they exist, the bar gets
// no global regime at all -- and never will: next_bar_to_compute() skips a bar
// once its cross-venue vector is written. While the sweep was slow this was
// hidden; once it kept up with the tape (2026-09-24, 15:19-15:44 UTC), 297 of
// 675 bars were computed before any venue regime existed -- they arrived ~12 s
// later -- and 10 more voted with one to three venues of four.
//
// So a fresh bar waits for its four regimes, but only briefly: a venue whose
// regime does not arrive within the bound must not hold the others' bar back,
// and a backfilled bar (older than the bound) is computed with what exists, as
// before.

#include "alttech/domain/venue.hpp"

#include <cstddef>
#include <cstdint>

namespace alttech::crossvenue {

inline constexpr int64_t kRegimeWaitMs = 90'000;   // from the bar's close

[[nodiscard]] inline bool wait_for_venue_regimes(size_t regimes_ready, int64_t bar_open_ms,
                                                 int64_t now_ms) noexcept {
    constexpr int64_t kBarMs = 60'000;
    if (regimes_ready >= kAllVenues.size()) return false;
    return now_ms - (bar_open_ms + kBarMs) < kRegimeWaitMs;
}

} // namespace alttech::crossvenue
