#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace altml::labels {

constexpr int64_t kOneMinuteMs = 60'000;
constexpr int64_t kOneDayMs = 86'400'000;

// ---- the label store's horizon ------------------------------------------------
// altml.labels is partitioned by month over the window ensure_data_partitions
// provisions, and trimmed by retention (labels: a 40-day target). AltSpotter's
// candle history is older than both (it starts 2026-02-01), so "the first
// candle" is not a place a label can be written: an insert there finds no
// partition. Every label the builder writes starts at the store floor.
constexpr int kLabelPartitionBackfillDays = 160;
constexpr int kLabelPartitionLookaheadDays = 60;
constexpr int kLabelStoreDays = 41;   // the 40-day retention target, plus one

// UTC month start, as altml.ensure_month_partitions floors its lower bound.
[[nodiscard]] inline int64_t month_start_ms(int64_t ms) {
    using namespace std::chrono;
    const year_month_day ymd{floor<days>(sys_time<milliseconds>{milliseconds{ms}})};
    return duration_cast<milliseconds>(sys_days{ymd.year() / ymd.month() / 1}.time_since_epoch()).count();
}

struct PartitionWindow {
    int64_t from_ms = 0;   // the first provisioned month's start
    int64_t to_ms = 0;     // provisioned through the month holding this instant
};

// The months ensure_data_partitions creates for altml.labels.
[[nodiscard]] inline PartitionWindow label_partition_window(int64_t now_ms) {
    return {month_start_ms(now_ms - kLabelPartitionBackfillDays * kOneDayMs),
            now_ms + kLabelPartitionLookaheadDays * kOneDayMs};
}

[[nodiscard]] inline int64_t label_store_floor_ms(int64_t now_ms, int backfill_days) {
    return now_ms - static_cast<int64_t>(backfill_days) * kOneDayMs;
}

// Where the one-time initial ATR repair recomputes from: the instrument's first
// candle -- its first ATR warmup boundary -- when the store can hold a label
// there. Older than the floor, the store holds no such boundary and there is
// nothing to repair.
[[nodiscard]] inline std::optional<int64_t> initial_atr_repair_start_ms(
    std::optional<int64_t> first_candle_ms, int64_t floor_ms) {
    if (!first_candle_ms || *first_candle_ms < floor_ms) return std::nullopt;
    return first_candle_ms;
}

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
