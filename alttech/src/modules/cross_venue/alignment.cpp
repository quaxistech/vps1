#include "alignment.hpp"

#include <algorithm>

namespace alttech::crossvenue {

AlignmentDecision evaluate_bar_window(std::span<const VenueSample> samples,
                                      const AlignmentParams& params,
                                      double elapsed_since_close_ms, bool already_emitted) {
    AlignmentDecision d;
    bool bitget_present = false;
    int present_count = 0;
    // Skew is the spread between the participating venues' clock offsets. Two
    // venues whose clocks agree describe the same instant however long ago the
    // bar closed, so the bar's own age must not enter this.
    double lowest_offset = 0.0;
    double highest_offset = 0.0;
    for (const auto& s : samples) {
        const uint32_t bit = venue_mask_bit(s.venue);
        if (s.present && usable(s.quality)) {
            if (present_count == 0) {
                lowest_offset = s.clock_offset_ms;
                highest_offset = s.clock_offset_ms;
            } else {
                lowest_offset = std::min(lowest_offset, s.clock_offset_ms);
                highest_offset = std::max(highest_offset, s.clock_offset_ms);
            }
            d.venue_mask |= bit;
            ++present_count;
            if (is_primary(s.venue)) bitget_present = true;
            if (s.late) d.late_venue_mask |= bit;
            if (s.revised) d.revised_venue_mask |= bit;
        } else {
            d.missing_venue_mask |= bit;
        }
    }
    d.max_skew_ms = present_count > 1 ? highest_offset - lowest_offset : 0.0;

    const bool watermark_elapsed = elapsed_since_close_ms >= params.bar_watermark_timeout_ms;
    const bool all_four = present_count == 4;
    const bool skew_ok = d.max_skew_ms <= params.max_cross_venue_skew_ms;

    // A venue arriving (or a revision arriving) after the window was already
    // emitted must not silently mutate the past — it is a LATE_REVISION that
    // triggers recomputation.
    if (already_emitted && (d.late_venue_mask != 0 || d.revised_venue_mask != 0)) {
        d.status = AlignmentStatus::late_revision;
        d.ready = true;
        return d;
    }

    if (!bitget_present) {
        // Without the primary venue the window cannot be trusted; only declare
        // it INVALID once we have stopped waiting.
        d.status = watermark_elapsed ? AlignmentStatus::invalid : AlignmentStatus::waiting;
        d.ready = watermark_elapsed;
        return d;
    }

    if (all_four && skew_ok) {
        d.status = AlignmentStatus::strict_aligned;
        d.ready = true;
        return d;
    }

    if (watermark_elapsed) {
        // Bitget plus at least one reference is a usable degraded window.
        d.status = present_count >= 2 ? AlignmentStatus::degraded_aligned
                                      : AlignmentStatus::invalid;
        d.ready = true;
        return d;
    }

    d.status = AlignmentStatus::waiting;
    d.ready = false;
    return d;
}

} // namespace alttech::crossvenue
