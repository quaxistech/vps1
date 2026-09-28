#pragma once

// adaptive_strategy_allocator (§10, §16–§18). Two cooperating pieces:
//
//  * PosteriorEstimator — the BATCH maths run by the maintenance scheduler. It
//    turns hierarchical strategy statistics into a shrunk posterior edge with a
//    Bayesian hierarchy (global → +regime → +symbol group → +exact symbol) and
//    an exponential time-decay state, and derives a bounded adaptive weight.
//
//  * AdaptiveAllocator — the RUNTIME step that, given a candidate and a
//    posterior table (loaded from altstrategy.strategy_posteriors), attaches the
//    prior/posterior/uncertainty/ESS and folds the adaptive weight into the
//    candidate's adaptive_score. Small-sample local cells shrink toward their
//    parent so a strategy with two lucky wins does not dominate ranking.

#include "altstrategy/engine/decision_types.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace altstrategy::engine {

// One statistics cell: (effective) sample count + mean edge + variance.
struct StatCell {
    double n = 0;
    double mean_edge = 0;
    double var = 0.01;   // variance of per-trade net return
};

// Result of shrinking a local cell toward a parent estimate.
struct ShrinkResult {
    double posterior_mean = 0;
    double posterior_uncertainty = 1;
    double shrinkage_factor = 0;   // weight on the LOCAL estimate, 0..1
    double effective_sample_size = 0;
};

// Empirical-Bayes shrinkage: posterior = w*local + (1-w)*parent, w = n/(n+k).
// prior_strength k is the pseudo-count that anchors a thin cell to its parent.
[[nodiscard]] ShrinkResult shrink_to_parent(const StatCell& local, double parent_mean,
                                            double parent_var, double prior_strength);

struct AllocatorConfig {
    double prior_strength = 20.0;   // k pseudo-counts for shrinkage
    double edge_scale = 0.004;      // net-return scale that maps to unit weight delta
    double weight_min = 0.25;
    double weight_max = 2.5;
    double half_life_days = 30.0;   // time-decay half life for decayed stats
    double weight_gain = 1.0;
};

// The full hierarchical posterior for one (strategy, regime, symbol_group, pair,
// side, holding) cell, given the cell and its parent chain (most-general last).
class PosteriorEstimator {
public:
    explicit PosteriorEstimator(AllocatorConfig cfg = {}) : cfg_(cfg) {}

    // parents: ordered from immediate parent to the global prior. Each entry is
    // that tier's already-aggregated StatCell. `global_prior` anchors the top.
    [[nodiscard]] StrategyPosterior estimate(const std::string& strategy_id,
                                             const std::string& strategy_version,
                                             const StatCell& local,
                                             const std::vector<StatCell>& parents,
                                             double global_prior_edge) const;

    // Exponential time-decay of a stream of (event_time_ms, net_return) into a
    // decayed effective-N + decayed edge as-of now_ms.
    [[nodiscard]] StatCell decay(const std::vector<std::pair<int64_t, double>>& obs,
                                 int64_t now_ms) const;

    // Map a posterior edge + ESS to a bounded adaptive weight (§16).
    [[nodiscard]] double weight_from(double posterior_edge, double ess) const;

    [[nodiscard]] const AllocatorConfig& config() const { return cfg_; }

private:
    AllocatorConfig cfg_;
};

// Runtime posterior table with hierarchical fallback lookup.
class PosteriorTable {
public:
    void put(const StrategyPosterior& p) { by_key_[key_of(p)] = p; }

    // Most-specific-first fallback: exact symbol → symbol group → regime →
    // strategy global. Returns nullopt only if the strategy is entirely unseen.
    [[nodiscard]] std::optional<StrategyPosterior> best(
        const std::string& strategy_id, const std::string& strategy_version,
        const std::string& regime, const std::string& symbol_group, int64_t pair,
        Side side, HoldingClass holding) const;

    // The strategy-global tier — the row update_posteriors() writes with every
    // dimension generalised. Lifecycle decisions are made at this level: a
    // per-pair cell is far too thin to retire a strategy on.
    [[nodiscard]] const StrategyPosterior* strategy_global(const std::string& strategy_id) const;

    [[nodiscard]] size_t size() const { return by_key_.size(); }

private:
    static std::string key(const std::string& sid, const std::string& ver, const std::string& regime,
                           const std::string& sg, int64_t pair, Side side, HoldingClass hc);
    static std::string key_of(const StrategyPosterior& p);
    std::map<std::string, StrategyPosterior> by_key_;
};

class AdaptiveAllocator {
public:
    explicit AdaptiveAllocator(AllocatorConfig cfg = {}) : cfg_(cfg) {}

    // Attaches posterior fields and refines adaptive_score in place. symbol_group
    // is a coarse bucket (e.g. "majors"/"alts") the caller derives from the pair.
    void allocate(StrategyCandidate& c, const std::string& symbol_group,
                  const PosteriorTable& table) const;

private:
    AllocatorConfig cfg_;
};

} // namespace altstrategy::engine
