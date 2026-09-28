#pragma once

// outcome_attribution (§22). Decomposes a realized result into strategy edge,
// ranking choice, plan geometry, ML estimate, regime change, costs and (if a VPS2
// execution feedback exists) execution quality. Crucially, a pure execution
// problem is charged to execution_attribution — never silently to the strategy.

#include "altstrategy/engine/decision_types.hpp"

#include <optional>

namespace altstrategy::engine {

[[nodiscard]] OutcomeAttribution attribute_outcome(const TradeSignalProposal& proposal,
                                                   const MarketOutcome& market,
                                                   const std::optional<ExecutionFeedback>& exec,
                                                   double ranking_regret, bool regime_changed,
                                                   int64_t now_ms);

} // namespace altstrategy::engine
