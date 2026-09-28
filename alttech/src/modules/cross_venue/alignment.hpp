#pragma once
// Event-time bar-window alignment.
//
// A window is ready once all four venues have closed bars, or once a watermark
// timeout past the canonical close has elapsed. Its skew is the spread between
// the participating venues' clock offsets: that is what decides whether the
// venues are describing the same instant.

#include "analytics_types.hpp"

#include <cstdint>
#include <span>

namespace alttech::crossvenue {

struct AlignmentParams {
    double bar_watermark_timeout_ms = 4000.0; // wait past close for late venues
    double max_cross_venue_skew_ms = 2500.0;
};

struct AlignmentDecision {
    AlignmentStatus status = AlignmentStatus::waiting;
    bool ready = false;               // window may be emitted now
    uint32_t venue_mask = 0;          // venues with a usable closed bar
    uint32_t missing_venue_mask = 0;
    uint32_t late_venue_mask = 0;
    uint32_t revised_venue_mask = 0;
    double max_skew_ms = 0.0;      // spread of participating venues' clock offsets
};

// Evaluate one canonical bar window. `elapsed_since_close_ms` is how long past
// the canonical close the watermark has run; `already_emitted` is true if the
// window was previously published (so a newly arriving venue is a revision).
[[nodiscard]] AlignmentDecision evaluate_bar_window(std::span<const VenueSample> samples,
                                                    const AlignmentParams& params,
                                                    double elapsed_since_close_ms,
                                                    bool already_emitted);

} // namespace alttech::crossvenue
