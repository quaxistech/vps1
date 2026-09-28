#include "altstrategy/engine/allocator.hpp"

#include <algorithm>
#include <cmath>

namespace altstrategy::engine {

ShrinkResult shrink_to_parent(const StatCell& local, double parent_mean, double parent_var,
                              double prior_strength) {
    ShrinkResult r;
    const double n = std::max(local.n, 0.0);
    const double w = n / (n + std::max(prior_strength, 1e-9));   // shrinkage weight
    r.shrinkage_factor = w;
    r.posterior_mean = w * local.mean_edge + (1.0 - w) * parent_mean;
    r.effective_sample_size = n;
    // Posterior variance blends the local sampling variance with the parent's
    // dispersion; thin cells inherit the parent's (wider) uncertainty.
    const double local_var = local.var / std::max(n, 1.0);
    const double pv = std::max(parent_var, 1e-9);
    r.posterior_uncertainty = std::sqrt(w * local_var + (1.0 - w) * pv);
    return r;
}

double PosteriorEstimator::weight_from(double posterior_edge, double ess) const {
    const double confidence = ess / (ess + cfg_.prior_strength);
    const double delta = cfg_.weight_gain * (posterior_edge / std::max(cfg_.edge_scale, 1e-9));
    const double w = 1.0 + confidence * delta;
    return std::clamp(w, cfg_.weight_min, cfg_.weight_max);
}

StrategyPosterior PosteriorEstimator::estimate(const std::string& strategy_id,
                                               const std::string& strategy_version,
                                               const StatCell& local,
                                               const std::vector<StatCell>& parents,
                                               double global_prior_edge) const {
    // Walk the chain from the top (global prior) down to the local cell so each
    // tier shrinks toward the already-shrunk tier above it.
    double parent_mean = global_prior_edge;
    double parent_var = 0.02;
    // Fold parents (given most-immediate first) from the general end inward.
    for (auto it = parents.rbegin(); it != parents.rend(); ++it) {
        const ShrinkResult s = shrink_to_parent(*it, parent_mean, parent_var, cfg_.prior_strength);
        parent_mean = s.posterior_mean;
        parent_var = s.posterior_uncertainty * s.posterior_uncertainty;
    }
    const ShrinkResult s = shrink_to_parent(local, parent_mean, parent_var, cfg_.prior_strength);

    StrategyPosterior p;
    p.strategy_id = strategy_id;
    p.strategy_version = strategy_version;
    p.strategy_prior = global_prior_edge;
    p.posterior_mean_edge = s.posterior_mean;
    p.posterior_uncertainty = s.posterior_uncertainty;
    p.effective_sample_size = s.effective_sample_size;
    p.shrinkage_factor = s.shrinkage_factor;
    p.sample_count = static_cast<int64_t>(std::llround(local.n));
    p.adaptive_weight = weight_from(s.posterior_mean, s.effective_sample_size);
    return p;
}

StatCell PosteriorEstimator::decay(const std::vector<std::pair<int64_t, double>>& obs,
                                   int64_t now_ms) const {
    StatCell c;
    if (obs.empty()) return c;
    const double half_life_ms = cfg_.half_life_days * 86400.0 * 1000.0;
    const double lambda = std::log(2.0) / std::max(half_life_ms, 1.0);
    double sw = 0, swx = 0, swx2 = 0;
    for (const auto& [t, x] : obs) {
        const double age = std::max(0.0, static_cast<double>(now_ms - t));
        const double w = std::exp(-lambda * age);
        sw += w;
        swx += w * x;
        swx2 += w * x * x;
    }
    c.n = sw;
    c.mean_edge = sw > 0 ? swx / sw : 0;
    c.var = sw > 0 ? std::max(swx2 / sw - c.mean_edge * c.mean_edge, 1e-6) : 0.01;
    return c;
}

// --------------------------- runtime table ----------------------------------

std::string PosteriorTable::key(const std::string& sid, const std::string& ver,
                                const std::string& regime, const std::string& sg, int64_t pair,
                                Side side, HoldingClass hc) {
    return sid + "|" + ver + "|" + regime + "|" + sg + "|" + std::to_string(pair) + "|" +
           to_string(side) + "|" + to_string(hc);
}

std::string PosteriorTable::key_of(const StrategyPosterior& p) {
    return key(p.strategy_id, p.strategy_version, p.regime, p.symbol_group, p.canonical_pair_id,
               p.side, p.holding_class);
}

const StrategyPosterior* PosteriorTable::strategy_global(const std::string& strategy_id) const {
    // Widest sample wins: the generalised tiers all carry the same strategy_id,
    // and the one aggregating every regime/pair/side is the one with the most
    // evidence behind it.
    const StrategyPosterior* best = nullptr;
    for (const auto& [k, p] : by_key_) {
        if (p.strategy_id != strategy_id) continue;
        if (!best || p.effective_sample_size > best->effective_sample_size) best = &p;
    }
    return best;
}

std::optional<StrategyPosterior> PosteriorTable::best(const std::string& sid, const std::string& ver,
                                                      const std::string& regime,
                                                      const std::string& sg, int64_t pair, Side side,
                                                      HoldingClass holding) const {
    // Try progressively more general keys.
    const std::vector<std::string> keys = {
        key(sid, ver, regime, sg, pair, side, holding),
        key(sid, ver, regime, sg, pair, side, HoldingClass::unspecified),
        key(sid, ver, regime, sg, 0, side, HoldingClass::unspecified),
        key(sid, ver, regime, sg, 0, Side::unspecified, HoldingClass::unspecified),
        key(sid, ver, regime, "*", 0, Side::unspecified, HoldingClass::unspecified),
        key(sid, ver, "*", "*", 0, Side::unspecified, HoldingClass::unspecified),
    };
    for (const auto& k : keys) {
        if (auto it = by_key_.find(k); it != by_key_.end()) return it->second;
    }
    return std::nullopt;
}

void AdaptiveAllocator::allocate(StrategyCandidate& c, const std::string& symbol_group,
                                 const PosteriorTable& table) const {
    auto post = table.best(c.strategy_id, c.strategy_version, c.global_regime, symbol_group,
                           c.canonical_pair_id, c.side, c.holding_class);
    double weight = 1.0;
    if (post) {
        c.strategy_prior = post->strategy_prior;
        c.strategy_posterior = post->posterior_mean_edge;
        c.posterior_uncertainty = post->posterior_uncertainty;
        c.effective_sample_size = post->effective_sample_size;
        weight = std::clamp(post->adaptive_weight, cfg_.weight_min, cfg_.weight_max);
    } else {
        // No history yet: neutral prior weight, wide uncertainty.
        c.posterior_uncertainty = 1.0;
        c.effective_sample_size = 0.0;
    }
    c.adaptive_score = std::max(0.0, c.static_score * weight);
}

} // namespace altstrategy::engine
