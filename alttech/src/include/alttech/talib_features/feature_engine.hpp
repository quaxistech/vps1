#pragma once
// Incremental-looking facade over the batch TA-Lib engine.
//
// TA-Lib is a batch library: every function consumes the whole series. This
// facade keeps a trailing window of bars and re-runs TA-Lib over it for each
// new bar, publishing the last row — so callers keep a simple "feed one bar,
// get one vector" contract while TA-Lib owns all indicator math.
//
// For bulk work use process_series(): a single TA-Lib pass over the whole
// history, which is what backfill wants (O(N) per function instead of O(N*W)).

#include "alttech/talib_features/engine.hpp"
#include "alttech/talib_features/types.hpp"

#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace alttech::talib_features {

class FeatureEngine {
public:
    // Bars replayed on restart, and the trailing window recomputed per bar.
    // Above TA-Lib's longest lookback at period 200 (~1194) so even the slowest
    // indicator is fully converged before anything is published.
    static constexpr size_t kWarmupBars = 2048;

    // A vector's STATUS answers one question: is enough of this vector usable
    // to act on? The per-feature truth is, and always was, the validity bitmap.
    //
    // The status used to be a pure bar count (>= 250 bars => VALID). That is a
    // proxy for "enough indicators have converged", and it is a bad one in both
    // directions. Measured on VPS1: a 1d vector built from ~75 bars carried 733
    // of 995 features valid — three quarters of the vector, every fast
    // indicator among them — and was published WARMING. AltML copies only
    // `status = 'VALID'`, and regime, structure and the cross-venue projection
    // all gate on it, so those 733 converged features were discarded because a
    // handful of long-lookback ones (T3 at period 200 needs ~1300 bars) had
    // not converged yet.
    //
    // The threshold is therefore on the FRACTION of features that carry a
    // value. 0.60 is where the fast ladder is complete: measured on a real
    // series, 50 bars yields 66% and 100 bars yields 80%, while a handful of
    // bars stays well below it. A slow feature can no longer hold the whole
    // vector hostage, and a vector with almost nothing in it is still WARMING.
    static constexpr double kMinUsableFraction = 0.60;

    // A floor beneath the fraction test, so the first few bars of a fresh
    // series are never published as VALID on a technicality.
    static constexpr size_t kMinValidBars = 20;

    FeatureEngine() = default;

    static size_t feature_count();

    // Feed the next closed bar (strictly increasing open_time).
    FeatureVector process_bar(const FBar& bar);

    // One TA-Lib pass over an entire series; returns a vector per input bar.
    // Does not touch this instance's rolling state.
    static std::vector<FeatureVector> process_series(const std::vector<FBar>& bars);

    // Restores the incremental facade from an ordered, contiguous trailing
    // replay. Only the last kWarmupBars are retained, exactly as if the bars had
    // arrived through process_bar(). The operation has a strong exception/error
    // guarantee: invalid ordering leaves the previous state untouched.
    Result<void> restore_state(std::span<const FBar> bars, int64_t span_ms);

    [[nodiscard]] size_t bars_seen() const { return bars_seen_; }
    [[nodiscard]] int64_t last_open_time_ms() const { return last_open_ms_; }

private:
    std::deque<FBar> window_;
    size_t bars_seen_ = 0;
    int64_t last_open_ms_ = 0;
};

} // namespace alttech::talib_features
