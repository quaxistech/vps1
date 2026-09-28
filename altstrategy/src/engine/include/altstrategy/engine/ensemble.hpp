#pragma once

// strategy_ensemble (§15). Combines the independent candidates for one context
// into a directional view, being careful (§11) NOT to count several strategies
// of the same edge family as independent confirmations.

#include "altstrategy/engine/decision_types.hpp"

#include <vector>

namespace altstrategy::engine {

// Score used as each candidate's vote weight (adaptive if set, else static).
[[nodiscard]] double vote_weight(const StrategyCandidate& c);

[[nodiscard]] EnsembleView compute_ensemble(const std::vector<StrategyCandidate>& candidates);

} // namespace altstrategy::engine
