#pragma once

// strategy_ranking (§19, §20). A versioned RankingPolicy turns each candidate's
// component vector (static + adaptive score, posterior edge/uncertainty/ESS,
// ensemble family agreement, ML direction/return/excursion/uncertainty/OOD,
// regime alignment, cross-venue confirmation, costs, structure room, fill
// probability, horizon, quality warnings) into one final score. The champion
// policy produces the recommended ranking; a challenger runs shadow and only
// replaces the champion after controlled validation + rollback readiness.

#include "altstrategy/engine/decision_types.hpp"

#include <map>
#include <string>
#include <vector>

namespace altstrategy::engine {

struct RankingPolicy {
    std::string ranking_policy_id = "champion_ml_v2";
    std::string ranking_policy_version = "1.0.0";
    std::map<std::string, double> weights;
    std::string normalization_version = "v1";
    std::string calibration_version = "v1";
    bool is_champion = true;
};

// Built-in default policies.
[[nodiscard]] RankingPolicy default_champion_policy();
[[nodiscard]] RankingPolicy default_challenger_policy();

// Deterministic component vector for one candidate given its context + ensemble.
[[nodiscard]] std::map<std::string, double> ranking_components(const StrategyCandidate& c,
                                                              const DecisionContext& ctx,
                                                              const EnsembleView& ensemble);

// Weighted score from components under a policy.
[[nodiscard]] double policy_score(const RankingPolicy& policy,
                                  const std::map<std::string, double>& components);

// Rank all candidates under a policy and classify the result. `close_gap` is the
// score gap below which the top two are treated as not-clearly-separated.
[[nodiscard]] RankedProposal rank(const std::vector<StrategyCandidate>& candidates,
                                  const DecisionContext& ctx, const EnsembleView& ensemble,
                                  const RankingPolicy& policy, int64_t now_ms,
                                  double close_gap = 0.08);

} // namespace altstrategy::engine
