#include "alttech/talib_features/feature_engine.hpp"

#include <algorithm>
#include <set>
#include <string_view>
#include <cmath>
#include <cstddef>
#include <limits>

namespace alttech::talib_features {
namespace {

// Builds the column-major arrays TA-Lib consumes from a bar sequence.
Ohlcv to_ohlcv(const std::deque<FBar>& bars) {
    Ohlcv o;
    o.open.reserve(bars.size());
    o.high.reserve(bars.size());
    o.low.reserve(bars.size());
    o.close.reserve(bars.size());
    o.volume.reserve(bars.size());
    for (const auto& b : bars) {
        o.open.push_back(b.open);
        o.high.push_back(b.high);
        o.low.push_back(b.low);
        o.close.push_back(b.close);
        o.volume.push_back(b.base_volume);
    }
    return o;
}

// Indicators that ACCUMULATE from the first bar of the series rather than over
// a fixed lookback: AD, ADOSC and OBV.
//
// TA-Lib returns a finite number for these from the very first bar, so the
// usual "finite means valid" rule publishes them long before they mean
// anything. The engine feeds a SLIDING window (kWarmupBars), so the value is
// an accumulation from wherever that window currently starts. Measured on one
// real instrument, the same bar yields:
//
//     100 bars -> AD  4 248 063      OBV -21 878 598
//     200 bars -> AD 11 462 659      OBV -21 834 133
//     400 bars -> AD  5 217 225      OBV -19 521 529
//    2048 bars -> AD 22 467 586      OBV -266 735 840
//
// The level therefore says as much about how much history happened to be
// loaded as about the market, and OBV is one of the twelve features AltML's
// t0_projection reads. Until the window is FULL, every bar accumulates over a
// different span and the series is not comparable with itself, so these stay
// invalid rather than carrying a number that looks usable.
[[nodiscard]] const std::vector<size_t>& accumulating_indices() {
    static const std::vector<size_t> idx = [] {
        static const std::set<std::string_view> kAccumulating = {
            "AD_outReal", "ADOSC_outReal", "OBV_outReal"};
        std::vector<size_t> out;
        const auto& names = core_feature_names();
        for (size_t i = 0; i < names.size(); ++i) {
            if (kAccumulating.contains(names[i])) out.push_back(i);
        }
        return out;
    }();
    return idx;
}

// Turns one computed row into the durable vector representation: NaN means the
// indicator has no value here (warm-up or unavailable input), so its validity
// bit stays 0 and the stored value is a benign 0.
FeatureVector to_vector(const std::vector<double>& row, int64_t open_time_ms, bool warm,
                        bool window_full) {
    FeatureVector fv;
    fv.bar_open_time_ms = open_time_ms;
    fv.values.assign(row.size(), 0.0F);
    fv.validity.assign((row.size() + 7U) / 8U, 0);
    size_t usable = 0;
    for (size_t i = 0; i < row.size(); ++i) {
        if (std::isfinite(row[i])) {
            fv.values[i] = static_cast<float>(row[i]);
            fv.validity[i / 8] |= static_cast<uint8_t>(1U << (i % 8));
            ++usable;
        }
    }
    if (!window_full) {
        for (const size_t i : accumulating_indices()) {
            if (i >= row.size()) continue;
            if ((fv.validity[i / 8] >> (i % 8)) & 1U) {
                fv.validity[i / 8] &= static_cast<uint8_t>(~(1U << (i % 8)));
                fv.values[i] = 0.0F;
                --usable;
            }
        }
    }
    // VALID means "enough of this vector converged to act on", not "the series
    // is long enough". See FeatureEngine::kMinUsableFraction.
    const double usable_fraction =
        row.empty() ? 0.0 : static_cast<double>(usable) / static_cast<double>(row.size());
    fv.status = (warm && usable_fraction >= FeatureEngine::kMinUsableFraction)
                    ? VectorStatus::valid
                    : VectorStatus::warming;
    return fv;
}

} // namespace

size_t FeatureEngine::feature_count() { return talib_features::feature_count(); }

FeatureVector FeatureEngine::process_bar(const FBar& bar) {
    window_.push_back(bar);
    while (window_.size() > kWarmupBars) window_.pop_front();
    ++bars_seen_;
    last_open_ms_ = bar.open_time_ms;

    const Ohlcv in = to_ohlcv(window_);
    std::vector<std::vector<double>> rows;
    const Engine* e = shared_engine();
    if (e == nullptr || !e->compute(in, rows) || rows.empty()) {
        FeatureVector fv;
        fv.bar_open_time_ms = bar.open_time_ms;
        fv.values.assign(feature_count(), 0.0F);
        fv.validity.assign((feature_count() + 7U) / 8U, 0);
        fv.status = VectorStatus::invalid;
        return fv;
    }
    // Coarse readiness; per-feature bits still gate each indicator individually.
    const bool warm = window_.size() >= kMinValidBars;
    return to_vector(rows.back(), bar.open_time_ms, warm, window_.size() >= kWarmupBars);
}

std::vector<FeatureVector> FeatureEngine::process_series(const std::vector<FBar>& bars) {
    std::vector<FeatureVector> out;
    if (bars.empty()) return out;

    std::deque<FBar> dq(bars.begin(), bars.end());
    const Ohlcv in = to_ohlcv(dq);
    std::vector<std::vector<double>> rows;
    const Engine* e = shared_engine();
    if (e == nullptr || !e->compute(in, rows) || rows.size() != bars.size()) return out;

    out.reserve(bars.size());
    for (size_t i = 0; i < bars.size(); ++i) {
        out.push_back(to_vector(rows[i], bars[i].open_time_ms, i + 1 >= kMinValidBars,
                                i + 1 >= kWarmupBars));
    }
    return out;
}

Result<void> FeatureEngine::restore_state(std::span<const FBar> bars, int64_t span_ms) {
    if (span_ms <= 0) {
        return fail(ErrorCode::invalid_argument,
                    "feature restore interval must be positive");
    }
    for (size_t i = 0; i < bars.size(); ++i) {
        const bool invalid_first = bars[i].open_time_ms < 0;
        const bool overflow = i > 0 &&
                              bars[i - 1].open_time_ms >
                                  std::numeric_limits<int64_t>::max() - span_ms;
        const bool discontinuity =
            i > 0 && !overflow &&
            bars[i].open_time_ms != bars[i - 1].open_time_ms + span_ms;
        if (invalid_first || overflow || discontinuity) {
            return fail(ErrorCode::invalid_argument,
                        "feature restore bars must form one contiguous interval");
        }
    }

    const size_t retained = std::min(bars.size(), kWarmupBars);
    const auto first = bars.end() - static_cast<std::ptrdiff_t>(retained);
    std::deque<FBar> restored(first, bars.end());

    window_.swap(restored);
    bars_seen_ = bars.size();
    last_open_ms_ = bars.empty() ? 0 : bars.back().open_time_ms;
    return {};
}

} // namespace alttech::talib_features
