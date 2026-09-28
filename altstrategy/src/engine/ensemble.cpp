#include "altstrategy/engine/ensemble.hpp"

#include "altstrategy/engine/ids.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace altstrategy::engine {

double vote_weight(const StrategyCandidate& c) {
    const double w = c.adaptive_score > 0 ? c.adaptive_score : c.static_score;
    return std::clamp(w, 0.0, 1.0);
}

EnsembleView compute_ensemble(const std::vector<StrategyCandidate>& candidates) {
    EnsembleView e;
    if (candidates.empty()) return e;

    double long_w = 0, short_w = 0;
    for (const auto& c : candidates) {
        if (c.side == Side::long_) long_w += vote_weight(c);
        else if (c.side == Side::short_) short_w += vote_weight(c);
    }
    e.ensemble_direction = long_w >= short_w ? Side::long_ : Side::short_;
    if (long_w == 0 && short_w == 0) e.ensemble_direction = Side::unspecified;

    const double support_w = e.ensemble_direction == Side::long_ ? long_w : short_w;
    const double conflict_w = e.ensemble_direction == Side::long_ ? short_w : long_w;
    e.ensemble_agreement = (support_w + conflict_w) > 0 ? support_w / (support_w + conflict_w) : 0;

    std::set<Family> sup_fam, con_fam;
    std::vector<double> sup_geo;
    for (const auto& c : candidates) {
        if (c.side == e.ensemble_direction) {
            e.supporting_strategies.push_back(c.strategy_id);
            sup_fam.insert(c.strategy_family);
            if (c.entry_hint > 0)
                sup_geo.push_back((c.target_hint - c.entry_hint) / c.entry_hint);
        } else if (c.side == opposite(e.ensemble_direction)) {
            e.conflicting_strategies.push_back(c.strategy_id);
            con_fam.insert(c.strategy_family);
        }
    }
    for (auto f : sup_fam) e.supporting_families.push_back(f);
    for (auto f : con_fam) e.conflicting_families.push_back(f);

    // Independence: distinct supporting families / supporting strategies. A pile
    // of same-family strategies scores low even if numerous (§11).
    const size_t sup_n = e.supporting_strategies.size();
    e.family_independence_score =
        sup_n > 0 ? static_cast<double>(sup_fam.size()) / static_cast<double>(sup_n) : 0;

    // Plan dispersion: coefficient of variation of implied reward among supporters.
    if (sup_geo.size() >= 2) {
        double mean = 0;
        for (double g : sup_geo) mean += g;
        mean /= static_cast<double>(sup_geo.size());
        double var = 0;
        for (double g : sup_geo) var += (g - mean) * (g - mean);
        var /= static_cast<double>(sup_geo.size());
        e.plan_dispersion = std::abs(mean) > 1e-9 ? std::sqrt(var) / std::abs(mean) : 0;
    }

    // Deterministic ensemble id from the participating candidate ids.
    std::string nk;
    for (const auto& c : candidates) nk += c.candidate_id + ";";
    e.ensemble_id = ids::make("ens", nk);
    return e;
}

} // namespace altstrategy::engine
