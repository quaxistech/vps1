#pragma once

// The market view a VPS1 signal carries: AltML's prediction on every served
// horizon, whatever the setup and whatever any economic diagnostic says.
//
// A signal is an event ("a 15m breakout, long, here") plus what the model
// expects the market to do after this state -- up / flat / down and how far,
// over 15 minutes, an hour and four hours. VPS2 decides whether that expected
// move is worth today's costs; VPS1 only has to make the view complete,
// point-in-time and honest about its condition (HorizonPrediction::state).

#include "altstrategy/engine/decision_types.hpp"

#include <cstdint>
#include <vector>

namespace altstrategy::engine {

// The horizons AltML serves. A horizon without a forecast is still reported,
// as MISSING, so a consumer can tell "no model" from "not asked".
inline constexpr int32_t kServedHorizons[] = {15, 60, 240};

[[nodiscard]] HorizonPrediction horizon_prediction(const DecisionContext& ctx, int32_t horizon_minutes,
                                                   int64_t now_ms, int64_t max_forecast_age_ms);

[[nodiscard]] std::vector<HorizonPrediction> market_predictions(const DecisionContext& ctx, int64_t now_ms,
                                                                int64_t max_forecast_age_ms);

} // namespace altstrategy::engine
