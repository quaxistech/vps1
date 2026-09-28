#pragma once

#include <cstdint>
#include <vector>

namespace altspotter::funding {

// Which REST ranges one hourly funding-settlement pass fetches for one
// instrument. Kept pure so the defect it was extracted from stays covered.
//
// The pass used to choose ONE of three branches. When the stored history was
// shallower than `funding_backfill_days`, it fetched only the missing OLDER
// slice -- and a venue whose own history is shorter than the configured depth
// (Bitget keeps ~90 days, the setting is 180) answers that request with an
// empty page every time. The forward catch-up sat in the other branch and was
// never reached: on VPS1 Bitget and Bybit received no settlement after
// 2026-08-31 04:00, while every hourly pass logged `empty_responses: 27`.
//
// New settlements are what every downstream consumer needs; a deeper history
// is a nice-to-have. So the forward range is planned first and independently,
// and the deepening range is planned only until the venue has shown that it
// has nothing older.

struct FetchRange {
    int64_t from_ms = 0;   // exclusive lower bound, as the adapter takes it
    int64_t to_ms = 0;     // inclusive upper bound
    bool deepen = false;   // true = the older slice below the stored history
};

inline constexpr int64_t kForwardMinGapMs = 3'600'000;   // settlements are hours apart
inline constexpr int64_t kDepthSlackMs = 86'400'000;     // one day: the target moves with time

// newest/oldest: the stored settlement range for the instrument (0 = none).
// target_from: now - funding_backfill_days. older_exhausted: the venue already
// answered the deepening request with nothing during this process's life.
[[nodiscard]] inline std::vector<FetchRange> plan_fetches(int64_t newest, int64_t oldest,
                                                          int64_t now, int64_t target_from,
                                                          bool older_exhausted) {
    std::vector<FetchRange> out;
    if (newest == 0) {
        out.push_back({target_from, now, false});   // nothing stored: the whole window
        return out;
    }
    if (now - newest >= kForwardMinGapMs) out.push_back({newest, now, false});
    if (!older_exhausted && oldest > target_from + kDepthSlackMs) {
        out.push_back({target_from, oldest - 1, true});
    }
    return out;
}

} // namespace altspotter::funding
