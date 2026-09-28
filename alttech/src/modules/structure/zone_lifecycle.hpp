#pragma once
// The market-structure lifecycle, as a pure function of a bar window.
//
// This is deliberately separate from the module: a zone's whole state must be
// derivable from the bars, never accumulated across cycles. AltSpotter revises
// 5-8 % of closed bars, with a 95th percentile around seven hours after close,
// so an accumulator drifts away from the corrected data and never returns,
// while a derivation simply yields the corrected answer next cycle.
//
// Two rules make the derivation mean what a trader means, and both were arrived
// at by measuring the alternatives on live data rather than by reasoning alone:
//
//   * A break is an event that happens *after* the level was recognised. Judging
//     it over the whole window instead marks almost everything broken — measured
//     at 53 % of 15m zones, 74 % of 1h and 79 % of 4h, because a level is a
//     level precisely because price has traded through that region before.
//   * A band price has *already* left is not a broken level, it is a level that
//     should never have been created. Measured, 23-30 % of zones sit beyond the
//     current close at birth. Rejecting them at creation keeps "broken" meaning
//     "failed while we were watching", and leaves distance to the strength
//     score, which decays with it.
//
// Touches are counted over the whole window, including before creation: how
// often the market respected a band is the evidence that it is a level at all.
// Breaks are not, because a break is about the level failing under observation.

#include <cstdint>
#include <string_view>
#include <vector>

namespace alttech::modules::structure {

// Geometry, all relative to ATR so no constant depends on a price level.
inline constexpr double kBreakAtrFraction = 0.5;    // a close beyond the band by this breaks it
inline constexpr double kReactionAtrFraction = 0.5; // a rejection of this size counts

struct SBar {
    int64_t open_ms = 0;
    double open = 0, high = 0, low = 0, close = 0, volume = 0, quote = 0;
    int64_t revision = 0;
};

// A price band. `created_ms` is when the zone was first recognised; 0 means a
// candidate that does not exist yet, for which no break can have occurred.
struct ZoneBand {
    double lower = 0;
    double center = 0;
    double upper = 0;
    std::string_view kind;  // support | resistance | dynamic
    int64_t created_ms = 0;
};

// The complete lifecycle state of one zone, derived from a bar window.
struct ZoneState {
    int touch_count = 0;      // bars whose range intersected the band
    int reaction_count = 0;   // touches the price then rejected by >= half an ATR
    bool broken = false;      // a bar closed decisively through it, after creation
    bool touched_recently = false;
    int64_t last_touch_ms = 0;
};

// True when `close` sits decisively beyond the band on the side that invalidates
// this kind of zone. Used both for the break test and, at creation time, to
// refuse a level price has already left behind.
[[nodiscard]] inline bool close_is_beyond(double close, const ZoneBand& band, double atr) {
    const double margin = kBreakAtrFraction * atr;
    return band.kind == "support" ? close < band.lower - margin
                                  : close > band.upper + margin;
}

// Evaluate a zone against bars[from..end). Pure: the same window always yields
// the same state, which is what makes recomputing after a source revision safe
// rather than additive.
[[nodiscard]] inline ZoneState evaluate_zone(const std::vector<SBar>& bars, std::size_t from,
                                             const ZoneBand& band, double atr) {
    ZoneState state;
    const double reaction_margin = kReactionAtrFraction * atr;
    const bool support = band.kind == "support";
    for (std::size_t i = from; i < bars.size(); ++i) {
        const SBar& bar = bars[i];
        if (bar.low <= band.upper && bar.high >= band.lower) {
            ++state.touch_count;
            state.last_touch_ms = bar.open_ms;
            const double reaction = support ? bar.close - band.center
                                            : band.center - bar.close;
            if (reaction > reaction_margin) ++state.reaction_count;
        }
        // Only bars after the level was recognised can break it.
        if (bar.open_ms > band.created_ms && close_is_beyond(bar.close, band, atr)) {
            state.broken = true;
        }
    }
    if (!bars.empty()) {
        const SBar& last = bars.back();
        state.touched_recently = last.low <= band.upper && last.high >= band.lower;
    }
    return state;
}

// Whether a zone older than the visible window keeps a break that has scrolled
// out of view. The window always spans far more than the revision horizon
// (120 bars is 30 hours at 15m, against a 7-hour revision tail), so anything a
// revision could still change is re-derived; only evidence too old to be revised
// is taken on trust, which is what stops a zone resurrecting.
[[nodiscard]] inline bool retains_break(bool derived_broken, bool stored_broken,
                                        int64_t created_ms, int64_t window_start_ms) {
    return derived_broken || (stored_broken && created_ms < window_start_ms);
}

} // namespace alttech::modules::structure
