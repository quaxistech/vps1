#pragma once

// The checks a forecast passes before its numbers are read -- one definition,
// shared by the market predictions a signal carries (signal_predictions.hpp)
// and the economic diagnostic (economic_policy.hpp), so the two can never
// disagree about whether an estimate was usable, fresh or complete.

#include "altstrategy/engine/decision_types.hpp"

#include <cmath>
#include <cstdint>

namespace altstrategy::engine::fc {

// A readable econ forecast: three class probabilities summing to one, finite
// per-side estimates, a positive cost, an anchor bar.
[[nodiscard]] inline bool usable(const MarketForecastView& f) {
    return f.econ && std::isfinite(f.p_long) && std::isfinite(f.p_short) &&
           std::isfinite(f.p_neutral) && std::isfinite(f.expected_net_long_bps) &&
           std::isfinite(f.expected_net_short_bps) && std::isfinite(f.round_trip_cost_bps) &&
           f.p_long >= 0 && f.p_short >= 0 && f.p_neutral >= 0 &&
           std::abs(f.p_long + f.p_short + f.p_neutral - 1.0) < 1e-3 &&
           f.round_trip_cost_bps > 0 && f.bar_open_time_ms > 0;
}

// A SHADOW model's estimate: information, labelled as such.
[[nodiscard]] inline bool shadow(const MarketForecastView& f) { return f.quality_status == "SHADOW"; }

// Scored while some of the model's aligned inputs (the Bitget 15m / 1h
// blocks) were absent. LightGBM takes the NaN branch and answers, but for an
// input the training rows almost never had.
[[nodiscard]] inline bool incomplete(const MarketForecastView& f) { return f.aligned_missing > 0; }

// Age measured from the close of the 1m bar the estimate was made on.
[[nodiscard]] inline bool fresh(const MarketForecastView& f, int64_t now_ms, int64_t max_age_ms) {
    const int64_t close_ms = f.bar_open_time_ms + 60'000;
    return now_ms - close_ms <= max_age_ms;
}

} // namespace altstrategy::engine::fc
