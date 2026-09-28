#pragma once
// Pure logic of alttech-dataset: the historical reconstruction of feature
// vectors from raw 1m candles, with the SAME aggregation and the SAME engine
// pass the live service uses, so a training row built from months of history
// holds the numbers production would have published for that bar.
//
// Aggregation is AltSpotter's (components/bar_aggregation.cpp): UTC windows on
// an epoch origin, open = first 1m open, high = max, low = min, close = last
// 1m close, volumes summed, and a window is 'ok' only when every one of its
// minutes is present. The engine pass is AltTech's backfill
// (feature_module.cpp): contiguous runs computed with process_series() in
// chunks, each chunk carrying the trailing kWarmupBars of context; a gap
// breaks the run and discards the context; an under-filled bar stays in the
// context but publishes no values of its own.

#include "alttech/talib_features/types.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace alttech::dataset {

struct MinuteBar {
    int64_t open_time_ms = 0;
    double open = 0, high = 0, low = 0, close = 0;
    double base_volume = 0, quote_volume = 0;
    bool ok = true;   // AltSpotter quality_status = 'ok' on a closed candle
};

struct TfBar {
    talib_features::FBar bar;
    bool ok = false;
};

// 1m -> tf, AltSpotter's rules. `minutes` is the tf span in minutes; 1 returns
// the 1m series itself (quality from the candle). Input must be ordered.
[[nodiscard]] inline std::vector<TfBar> aggregate(const std::vector<MinuteBar>& in, int minutes) {
    std::vector<TfBar> out;
    if (minutes <= 1) {
        out.reserve(in.size());
        for (const auto& m : in) {
            out.push_back({{m.open_time_ms, m.open, m.high, m.low, m.close, m.base_volume,
                            m.quote_volume},
                           m.ok});
        }
        return out;
    }
    const int64_t span = static_cast<int64_t>(minutes) * 60'000;
    size_t i = 0;
    while (i < in.size()) {
        const int64_t win = in[i].open_time_ms - (((in[i].open_time_ms % span) + span) % span);
        TfBar b;
        b.bar.open_time_ms = win;
        b.bar.open = in[i].open;
        b.bar.high = in[i].high;
        b.bar.low = in[i].low;
        int count = 0;
        while (i < in.size() && in[i].open_time_ms < win + span) {
            const auto& m = in[i];
            if (m.high > b.bar.high) b.bar.high = m.high;
            if (m.low < b.bar.low) b.bar.low = m.low;
            b.bar.close = m.close;
            b.bar.base_volume += m.base_volume;
            b.bar.quote_volume += m.quote_volume;
            ++count;
            ++i;
        }
        b.ok = count == minutes;
        out.push_back(b);
    }
    return out;
}

// Whether a bar's CLOSE falls on the emission grid, and the bar is inside the
// requested range. A decision at time T reads the bar that closed at or before
// T, so the grid is on close times.
[[nodiscard]] inline bool emit_bar(int64_t open_time_ms, int64_t span_ms, int64_t every_ms,
                                   int64_t from_ms, int64_t to_ms) {
    if (open_time_ms < from_ms || open_time_ms >= to_ms) return false;
    if (every_ms <= span_ms) return true;
    return ((open_time_ms + span_ms) % every_ms) == 0;
}

} // namespace alttech::dataset
