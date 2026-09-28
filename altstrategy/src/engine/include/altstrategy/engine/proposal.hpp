#pragma once

// proposal_publisher (§12, §13). Turns a chosen plan geometry + its AltML
// evaluation into a concrete TradeSignalProposal: assigns the grade, folds ML /
// quality weaknesses into warnings + risk flags (never a hard reject), and runs
// the ONE hard gate — technical INVALID (§13) — that blocks publication as an
// executable proposal. It never sets size/leverage/order id.

#include "altstrategy/engine/decision_types.hpp"

#include <string>
#include <vector>

namespace altstrategy::engine {

struct ProposalInputs {
    const StrategyCandidate& candidate;
    const TradePlanVariant& plan;        // the recommended geometry (usually BALANCED)
    const MlEvaluation& ml;
    const EnsembleView& ensemble;
    const DecisionContext& ctx;
    std::string ranking_id;
    std::vector<std::string> alternative_plan_ids;
    bool is_recommended_candidate = false;  // ranking picked this candidate
    int plan_version = 1;
};

// Reasons a proposal is technically INVALID (§13). Empty => valid to publish.
[[nodiscard]] std::vector<std::string> invalid_reasons(const TradePlanVariant& plan,
                                                       const DecisionContext& ctx, int64_t now_ms);

// Assign the grade given the (already computed) validity + candidate + scores.
[[nodiscard]] Grade assign_grade(const StrategyCandidate& c, const MlEvaluation& ml,
                                 const EnsembleView& ensemble, bool invalid);

// Build the concrete proposal. A technically INVALID proposal is returned with
// grade INVALID and recommended_for_execution=false so the caller can archive it
// but must NOT publish it as executable (§13).
[[nodiscard]] TradeSignalProposal build_proposal(const ProposalInputs& in, int64_t now_ms);

} // namespace altstrategy::engine
