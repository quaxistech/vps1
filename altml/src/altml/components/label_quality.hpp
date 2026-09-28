#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace altml::labels {

constexpr int64_t kOneMinuteMs = 60'000;

struct SourceBar {
    int64_t open_ms = 0;
    double open = 0;
    double high = 0;
    double low = 0;
    double close = 0;
    int64_t revision = 0;
    bool quality_ok = false;
};

// The caller may retain invalid bars in the ordered input so a bad minute is
// never silently compressed out of a future horizon.
[[nodiscard]] inline bool source_range_valid(const std::vector<SourceBar>& bars, size_t anchor,
                                             int horizon_minutes) {
    if (horizon_minutes < 0 || anchor >= bars.size() ||
        static_cast<size_t>(horizon_minutes) >= bars.size() - anchor) {
        return false;
    }
    const int64_t anchor_ms = bars[anchor].open_ms;
    for (int minute = 0; minute <= horizon_minutes; ++minute) {
        const auto& bar = bars[anchor + static_cast<size_t>(minute)];
        if (!bar.quality_ok ||
            bar.open_ms != anchor_ms + static_cast<int64_t>(minute) * kOneMinuteMs) {
            return false;
        }
    }
    return true;
}

struct AtrState {
    std::deque<double> true_ranges;
    int64_t previous_open_ms = 0;
    double previous_close = 0;

    void reset() {
        true_ranges.clear();
        previous_open_ms = 0;
        previous_close = 0;
    }

    // A bad bar or a missing minute breaks the causal ATR chain. The first
    // good bar after the break contributes only its own high-low range; a full
    // fourteen-bar contiguous window is required before ATR is valid again.
    void advance(const SourceBar& bar) {
        if (!bar.quality_ok) {
            reset();
            return;
        }
        if (previous_open_ms == 0 || bar.open_ms != previous_open_ms + kOneMinuteMs) {
            true_ranges.clear();
            previous_close = 0;
        }
        const double tr = previous_close > 0
                              ? std::max({bar.high - bar.low,
                                          std::abs(bar.high - previous_close),
                                          std::abs(bar.low - previous_close)})
                              : bar.high - bar.low;
        true_ranges.push_back(tr);
        if (true_ranges.size() > 14) true_ranges.pop_front();
        previous_open_ms = bar.open_ms;
        previous_close = bar.close;
    }

    [[nodiscard]] bool ready() const { return true_ranges.size() == 14; }

    [[nodiscard]] double value() const {
        if (!ready()) return 0;
        double sum = 0;
        for (const double tr : true_ranges) sum += tr;
        return sum / static_cast<double>(true_ranges.size());
    }
};

} // namespace altml::labels
