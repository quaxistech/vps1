#pragma once

// Full-state decisions: the model's view of the market state, whether or not a
// setup detector fired.
//
// The 16 strategies are setup DESCRIPTORS -- "this looks like a breakout /
// pullback / reclaim". Before this, a state could reach the economic policy
// only through one of them: AltML scored every instrument every minute, but an
// estimate that the next hour pays its costs was never acted on unless a
// detector's hand-set threshold happened to trigger on the same bar. The
// detectors decided which market states the model was allowed to be right
// about.
//
// Here every horizon and side of every context is put to the same economic
// policy. A state candidate exists only where that policy says the move pays
// beyond the model's validated margin (POSITIVE/STRONG_EDGE), or -- to record
// what a SHADOW model would have done -- where a shadow estimate clears its
// own margin. It is tagged with the setups that fired on the same bar and
// side, so a setup becomes a description of the state rather than its gate.
// Its trade geometry is the model's own: a hold of the horizon, invalidation
// at the expected adverse excursion, target at the expected favourable one.

#include "altstrategy/engine/decision_types.hpp"
#include "altstrategy/engine/economic_policy.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace altstrategy::engine {

inline constexpr const char* kStateStrategyId = "ml_state";
inline constexpr const char* kStateStrategyVersion = "1";

struct StateCandidateConfig {
    // OFF by default (2026-09-26): as built, a state candidate exists only
    // where the ECONOMIC estimate clears its margin -- an economic generator,
    // not a market event, and VPS1 takes no economic decision. Kept for the
    // record and for replay of the period it ran.
    bool enabled = false;
    int64_t candidate_ttl_ms = 15LL * 60 * 1000;
};

// `admit(horizon_minutes, side)` lets the caller suppress a repeat of a state
// candidate already raised within its horizon (one open thesis per
// instrument, horizon and side); it is consulted only for a candidate that
// would otherwise be created.
[[nodiscard]] std::vector<StrategyCandidate> state_candidates(
    const DecisionContext& ctx, int64_t now_ms, const EconomicPolicyConfig& econ,
    const StateCandidateConfig& cfg, const std::vector<StrategyCandidate>& setups,
    const std::function<bool(int32_t, Side)>& admit, int32_t only_horizon = 0);

} // namespace altstrategy::engine
