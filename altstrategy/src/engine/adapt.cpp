#include "altstrategy/engine/adapt.hpp"

#include "altstrategy/engine/ids.hpp"

#include <algorithm>
#include <cmath>

namespace altstrategy::engine {

namespace {
// Symmetric, BOUNDED relative difference in [0,1].
//
// The previous form was |recent-base| / (|base|+eps), which is unbounded: a
// baseline near zero sent it to ~1e9 and, since every drift threshold here lives
// on [0.2,0.8], any such strategy was classified STRUCTURAL_BREAK on arithmetic
// alone. That was not a marginal inaccuracy — `regime_perf` and `symbol_perf`
// are MEAN RETURNS, which are centred near zero by nature, so the two terms
// weighed most heavily in the verdict were the two guaranteed to blow up.
//
// This form is defined at zero, symmetric in its arguments, reaches 1.0 exactly
// when one side is zero or the two have equal magnitude and opposite sign (a
// sign flip in mean return being maximal drift), and keeps the thresholds
// meaningful because the measure and they share the same scale.
double rel(double base, double recent) {
    const double denom = std::abs(base) + std::abs(recent);
    if (denom < 1e-12) return 0.0;   // both flat at zero: no drift, not infinite
    return std::abs(recent - base) / denom;
}

// Drift in a MEAN, measured against the uncertainty of that mean.
//
// `rel()` above is right for quantities whose magnitude carries meaning. It is
// wrong for MEAN RETURNS, and wrongness here is not a matter of degree: because
// the measure is scale-free it returns exactly 1.0 for ANY sign flip, however
// small. Measured on this deployment, `momentum_continuation` scored
// regime_performance_drift = 0.977 — a STRUCTURAL_BREAK verdict — on mean
// returns that sit a couple of basis points either side of zero, while a
// deterioration from +0.010 to +0.004 (60 bps, economically large and
// statistically real) would have scored 0.43. The measure ranked the
// meaningless change above the meaningful one.
//
// That is not merely an inaccurate label. Recovery from DEGRADED requires
// `drift == stable`, so a noise-driven sign flip in a near-zero mean locks a
// strategy out of recovery permanently, no matter how its edge actually
// develops.
//
// A difference in means is judged against its standard error, which is what
// respects both effect size and sample size. The mapping z/(z+2) keeps the
// existing thresholds on their existing scale and gives them a statistical
// reading: ~1 sigma reaches the `degraded` band at 0.35, and 3 sigma reaches
// `structural_break` at 0.6.
double mean_drift(double base_mean, double base_sd, int64_t base_n, double recent_mean,
                  double recent_sd, int64_t recent_n) {
    // Without at least two observations per side there is no dispersion to
    // judge against, and inventing one would be the same error in a new place.
    if (base_n < 2 || recent_n < 2) return 0.0;
    const double se_squared =
        (base_sd * base_sd) / static_cast<double>(base_n) +
        (recent_sd * recent_sd) / static_cast<double>(recent_n);
    if (!(se_squared > 0.0)) {
        // Zero dispersion on both sides: identical means are no drift, and
        // different ones are a step change this measure cannot scale.
        return std::abs(recent_mean - base_mean) > 0.0 ? 1.0 : 0.0;
    }
    const double z = std::abs(recent_mean - base_mean) / std::sqrt(se_squared);
    return z / (z + 2.0);
}

// For quantities that are ALREADY probabilities on [0,1] — win rate, fill rate —
// the absolute gap is the honest measure; a relative one would call 0.02 -> 0.04
// a doubling.
double rate_diff(double base, double recent) {
    return std::clamp(std::abs(recent - base), 0.0, 1.0);
}
double clamp_step(double v, const StrategyParam& p) {
    v = std::clamp(v, p.min, p.max);
    if (p.step > 0) v = p.min + std::round((v - p.min) / p.step) * p.step;
    return std::clamp(v, p.min, p.max);
}
}

StrategyDrift compute_drift(const std::string& sid, const std::string& ver, const DriftInputs& in,
                            int64_t now_ms) {
    StrategyDrift d;
    d.strategy_id = sid;
    d.strategy_version = ver;
    d.computed_at_ms = now_ms;
    d.candidate_frequency_drift = rel(in.baseline_candidate_freq, in.recent_candidate_freq);
    d.score_distribution_drift = rel(in.baseline_score_mean, in.recent_score_mean);
    d.outcome_drift = rate_diff(in.baseline_win_rate, in.recent_win_rate);
    d.fill_time_drift = rate_diff(in.baseline_fill, in.recent_fill);
    d.holding_time_drift = rel(in.baseline_holding, in.recent_holding);
    d.mfe_mae_drift = 0.5 * (rel(in.baseline_mfe, in.recent_mfe) + rel(in.baseline_mae, in.recent_mae));
    d.regime_performance_drift =
        mean_drift(in.baseline_regime_perf, in.baseline_regime_perf_sd, in.baseline_regime_groups,
                   in.recent_regime_perf, in.recent_regime_perf_sd, in.recent_regime_groups);
    d.symbol_performance_drift =
        mean_drift(in.baseline_symbol_perf, in.baseline_symbol_perf_sd, in.baseline_symbol_groups,
                   in.recent_symbol_perf, in.recent_symbol_perf_sd, in.recent_symbol_groups);

    const double worst = std::max({d.candidate_frequency_drift, d.score_distribution_drift,
                                   d.outcome_drift, d.fill_time_drift, d.holding_time_drift,
                                   d.mfe_mae_drift, d.regime_performance_drift, d.symbol_performance_drift});
    // Outcome + performance drift weigh most heavily.
    const double critical = std::max({d.outcome_drift, d.regime_performance_drift, d.symbol_performance_drift});
    // Drift compares two windows, so a verdict requires evidence in BOTH of
    // them. The guard used to check only the combined count, which is the
    // wrong quantity: with 3,339 baseline outcomes and 0 recent ones it passed
    // comfortably, `recent_win_rate` defaulted to 0.0, and the resulting
    // "outcome drift" of 0.29 was nothing but the baseline win rate itself.
    //
    // That verdict was STRUCTURAL_BREAK, and it is self-reinforcing: a chain
    // that stalls produces no outcomes, the silence reads as a total collapse
    // in win rate, the strategies are held DEGRADED, and being DEGRADED means
    // they cannot generate the outcomes that would clear the verdict. The
    // system could not recover from an outage because the outage was being
    // counted as evidence against it.
    constexpr int64_t kMinWindowSamples = 30;
    const bool enough_evidence = in.baseline_sample_count >= kMinWindowSamples &&
                                 in.recent_sample_count >= kMinWindowSamples;
    if (!enough_evidence) d.status = DriftStatus::stable;   // not enough evidence
    else if (critical > 0.6 || worst > 0.8) d.status = DriftStatus::structural_break;
    else if (critical > 0.35 || worst > 0.5) d.status = DriftStatus::degraded;
    else if (worst > 0.2) d.status = DriftStatus::watch;
    else d.status = DriftStatus::stable;

    if (d.outcome_drift > 0.35) d.reasons.push_back("outcome_drift");
    if (d.candidate_frequency_drift > 0.5) d.reasons.push_back("candidate_frequency_drift");
    if (d.mfe_mae_drift > 0.4) d.reasons.push_back("excursion_drift");
    if (d.fill_time_drift > 0.4) d.reasons.push_back("fill_time_drift");
    return d;
}

std::map<std::string, double> calibrate_parameters(const std::vector<StrategyParam>& schema,
                                                   const std::map<std::string, double>& current,
                                                   const CalibrationTargets& t, double lr) {
    std::map<std::string, double> out = current;
    // Only calibrate with enough samples; otherwise leave the set unchanged (§33).
    if (t.sample_count < 30) return out;

    auto find = [&](const std::string& n) -> const StrategyParam* {
        for (const auto& p : schema) if (p.name == n) return &p;
        return nullptr;
    };
    auto adjust = [&](const std::string& name, double ratio) {
        auto* p = find(name);
        if (!p) return;
        const double cur = current.count(name) ? current.at(name) : p->default_value;
        // Move a bounded fraction toward cur*ratio.
        const double target = cur * ratio;
        out[name] = clamp_step(cur + lr * (target - cur), *p);
    };

    // Stop buffer: if actual MAE exceeded predicted, widen stop (ratio>1).
    if (t.predicted_mae > 0 && t.actual_mae > 0)
        adjust("stop_atr", std::clamp(t.actual_mae / t.predicted_mae, 0.7, 1.5));
    // Target: if actual MFE fell short of predicted, pull target in (ratio<1).
    if (t.predicted_mfe > 0 && t.actual_mfe > 0)
        adjust("target_atr", std::clamp(t.actual_mfe / t.predicted_mfe, 0.7, 1.5));
    return out;
}

std::optional<LifecycleTransition> evaluate_lifecycle(const std::string& sid, const std::string& ver,
                                                      const LifecycleInputs& in, int64_t now_ms) {
    auto mk = [&](Lifecycle to, const char* reason, bool rollback) {
        LifecycleTransition t;
        t.strategy_id = sid;
        t.strategy_version = ver;
        t.from_status = in.current;
        t.to_status = to;
        t.reason = reason;
        t.sample_count = in.sample_count;
        t.rollback = rollback;
        t.created_at_ms = now_ms;
        return std::optional<LifecycleTransition>(t);
    };
    if (!in.cooldown_elapsed) return std::nullopt;

    // Demotions can act on a structural break with fewer samples (safety first).
    if (in.current == Lifecycle::active) {
        if (in.drift == DriftStatus::structural_break ||
            (in.drift == DriftStatus::degraded && in.posterior_edge < 0 && in.sample_count >= in.min_samples))
            return mk(Lifecycle::degraded, "drift_or_negative_edge", false);
        // A MEASURED negative edge demotes on its own. ACTIVE used to be left
        // only through the drift detector, so a strategy losing steadily --
        // no drift, because it had always lost -- stayed ACTIVE forever.
        if (in.sample_count >= in.min_samples && in.posterior_edge < 0)
            return mk(Lifecycle::degraded, "measured_negative_edge", false);
        return std::nullopt;
    }
    // RESEARCH has an exit: the same evidence that promotes SHADOW moves a
    // research setup into SHADOW. It keeps collecting data either way --
    // research candidates are ranked, planned for observation and scored --
    // so a setup that never earns promotion is a measured "no", not a limbo.
    if (in.current == Lifecycle::research) {
        const bool evidenced = in.sample_count >= in.min_samples && in.posterior_edge > 0 &&
                               in.posterior_uncertainty < 0.01 && in.drift == DriftStatus::stable;
        if (evidenced) return mk(Lifecycle::shadow, "research_validated", false);
        return std::nullopt;
    }
    if (in.current == Lifecycle::degraded) {
        if (in.sample_count >= in.min_samples && in.posterior_edge < 0)
            return mk(Lifecycle::shadow, "sustained_negative_edge", false);
        // Recovery path back to active if edge recovered strongly.
        if (in.drift == DriftStatus::stable && in.posterior_edge > 0.001 && in.sample_count >= in.min_samples)
            return mk(Lifecycle::active, "recovered", true);
        return std::nullopt;
    }
    // Promotions require a full sample budget, low uncertainty and positive edge.
    const bool promotable = in.sample_count >= in.min_samples && in.posterior_edge > 0 &&
                            in.posterior_uncertainty < 0.01 && in.drift == DriftStatus::stable;
    if (in.current == Lifecycle::shadow && promotable) return mk(Lifecycle::candidate, "shadow_validated", false);
    if (in.current == Lifecycle::candidate && promotable) return mk(Lifecycle::limited, "candidate_validated", false);
    if (in.current == Lifecycle::limited && promotable && in.sample_count >= in.min_samples * 2)
        return mk(Lifecycle::active, "limited_validated", false);
    return std::nullopt;
}

Lifecycle current_lifecycle(const std::string& strategy_id,
                            const std::map<std::string, Lifecycle>& overrides,
                            const std::map<std::string, Lifecycle>& declared) {
    if (const auto it = overrides.find(strategy_id); it != overrides.end()) return it->second;
    if (const auto it = declared.find(strategy_id); it != declared.end()) return it->second;
    // A strategy the registry does not know cannot be reasoned about; ACTIVE is
    // the only status whose branch is purely defensive (it can demote, never
    // promote), so an unknown id can never be promoted by accident.
    return Lifecycle::active;
}

} // namespace altstrategy::engine
