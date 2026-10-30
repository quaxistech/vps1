#pragma once

// The economic decision policy (ML-first).
//
// A strategy is a SETUP DETECTOR: it says "this is a potential breakout /
// pullback / reclaim, long or short, over roughly this horizon". Whether acting
// on it pays is not the strategy's call. It is answered here, from AltML's
// calibrated econ estimate for the candidate's horizon:
//
//   expected net (this side) = calibrated class probabilities x what each class
//                              has been worth, minus the all-in round trip
//
// and a candidate is actionable only when that number clears the model's own
// validated margin above the estimated cost (POSITIVE_EDGE / STRONG_EDGE). The
// margin is chosen on the model's validation slice, never on its holdout, and
// travels with the model; a model without one is never actionable. Everything else is a
// named refusal: no estimate, a stale one, an incompatible one, an abstention,
// an estimate that says it does not pay, or a longer/shorter horizon that says
// the opposite side pays. There is no fallback to the old static score: with
// no usable model the policy fails CLOSED.

#include "altstrategy/engine/decision_types.hpp"

#include <cstdint>
#include <optional>

namespace altstrategy::engine {

struct EconomicPolicyConfig {
    // How old an estimate may be, measured from the close of the 1m bar it
    // was made on, when a decision reads it. AltML scores every 1m bar within
    // seconds of its close, so three bars of age means the stream stalled.
    int64_t max_forecast_age_ms = 180'000;
};

// Exact supported horizon only. Zero means no compatible forecast exists;
// setup duration/TTL is preserved and an 8h setup never receives a 4h payoff.
[[nodiscard]] int32_t evaluation_horizon_minutes(int64_t expected_horizon_ms);

[[nodiscard]] EconomicDecision evaluate_economics(Side side, int64_t expected_horizon_ms,
                                                  const DecisionContext& ctx, int64_t now_ms,
                                                  const EconomicPolicyConfig& cfg);

// The same verdict for an explicit model horizon (the full-state path, which
// has no setup horizon to map).
[[nodiscard]] EconomicDecision evaluate_economics_at(Side side, int32_t horizon_minutes,
                                                     const DecisionContext& ctx, int64_t now_ms,
                                                     const EconomicPolicyConfig& cfg);

// NO / SMALL / POSITIVE / STRONG from the expected net, the estimated round
// trip and the model's validated margin (see EconState).
[[nodiscard]] EconState classify_edge(double expected_net_bps, double round_trip_cost_bps,
                                      std::optional<double> margin_bps);

} // namespace altstrategy::engine
