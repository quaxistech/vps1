#pragma once

// The decision policy: turning a calibrated quality score into ACCEPT/REJECT,
// and choosing the threshold that does it.
//
// The threshold is not a constant in the source and not a number an operator
// guesses. It is FITTED on the validation slice, stored with the model that was
// fitted with it, and travels with that model — so rolling a model back rolls
// its threshold back too.
//
// What it is fitted to matters. Maximising accepted expectancy alone selects a
// threshold that accepts three signals a month: high mean, no business. The
// objective here is RETURN PER CANDIDATE OFFERED — total net return of the
// accepted set divided by every candidate the filter was shown — because that
// is the quantity the desk actually earns, and it is the one objective under
// which "reject everything" scores zero rather than infinity.
//
// A minimum acceptance rate and a minimum accepted count are still enforced, so
// a threshold cannot be chosen on a handful of lucky signals.
//
// Deliberately NOT implemented: per-regime thresholds. Regime is already a
// model feature, so a regime-specific cut adds a second, much noisier
// estimation problem (each regime would need its own validation sample) for an
// effect the model can express directly. See docs/PROMOTION_GATES.md.

#include "altsignalquality/domain/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace altsignalquality::domain {

struct ThresholdSearchConfig {
    double min_threshold = 0.05;
    double max_threshold = 0.95;
    int grid_points = 91;             // 0.05 .. 0.95 in 0.01 steps
    double min_acceptance_rate = 0.10;
    int64_t min_accepted = 30;
};

struct ThresholdChoice {
    bool found = false;
    // NaN, not 0.5. A caller that forgets to check `found` gets a value that
    // fails every comparison instead of one that silently accepts half the
    // signals it is shown.
    double threshold = std::numeric_limits<double>::quiet_NaN();
    double objective = std::nan("");   // return per candidate offered
    FilterEvaluation evaluation;
    std::string rejection;             // why no threshold qualified
};

// Search the grid on a slice the model did not fit. Ties go to the LOWER
// threshold: between two cuts that earn the same, the one that trades more is
// the one with more evidence behind it and less variance in production.
[[nodiscard]] inline ThresholdChoice choose_threshold(const std::vector<int>& y,
                                                      const std::vector<double>& score,
                                                      const std::vector<double>& net_return,
                                                      const ThresholdSearchConfig& cfg) {
    ThresholdChoice best;
    if (y.empty() || y.size() != score.size() || y.size() != net_return.size()) {
        best.rejection = "empty or mismatched calibration slice";
        return best;
    }
    if (cfg.grid_points < 2 || !(cfg.max_threshold > cfg.min_threshold)) {
        best.rejection = "invalid threshold grid";
        return best;
    }
    int64_t candidates_examined = 0;
    for (int g = 0; g < cfg.grid_points; ++g) {
        const double t = cfg.min_threshold +
                         (cfg.max_threshold - cfg.min_threshold) * static_cast<double>(g) /
                             static_cast<double>(cfg.grid_points - 1);
        const auto ev = evaluate_filter(y, score, net_return, t);
        if (ev.accepted.count < cfg.min_accepted) continue;
        if (ev.acceptance_rate < cfg.min_acceptance_rate) continue;
        ++candidates_examined;
        const double objective = ev.accepted.return_per_candidate;
        if (!std::isfinite(objective)) continue;
        if (!best.found || objective > best.objective) {
            best.found = true;
            best.threshold = t;
            best.objective = objective;
            best.evaluation = ev;
        }
    }
    if (!best.found) {
        best.rejection = candidates_examined == 0
                             ? "no threshold met min_accepted / min_acceptance_rate"
                             : "no threshold produced a finite objective";
    }
    return best;
}

// ---------------------------------------------------------------------------
// The live decision
// ---------------------------------------------------------------------------

// What the service does with a VALID signal. This is a policy choice with real
// consequences, so it is configured, logged on every use, and recorded on the
// evaluation row -- never an implicit default buried in a branch.
//
// PASS_THROUGH_RECOMMENDED is retired. It forwarded only what AltStrategy had
// recommended for execution, and that recommendation was an economic verdict
// (the model's expected net after costs): for seven days to 2026-09-26 it
// rejected 8431 of 8431 signals and VPS2 received nothing. VPS1 takes no
// execution decision -- `recommended_for_execution` is always false -- and
// this service decides validity, quality and contract correctness only.
enum class ColdStartPolicy {
    // Forward every VALID signal. The default and the only production policy.
    pass_through_all,
    // Forward nothing. Safe, and makes the pipeline produce no signals at all,
    // so it is only correct for a deliberate freeze.
    reject_all,
};

[[nodiscard]] inline std::string_view cold_start_policy_name(ColdStartPolicy p) noexcept {
    switch (p) {
        case ColdStartPolicy::pass_through_all: return "PASS_THROUGH_ALL";
        case ColdStartPolicy::reject_all: return "REJECT_ALL";
    }
    return "?";
}

[[nodiscard]] inline std::optional<ColdStartPolicy> parse_cold_start_policy(std::string_view s) {
    if (s == "PASS_THROUGH_ALL") return ColdStartPolicy::pass_through_all;
    if (s == "REJECT_ALL") return ColdStartPolicy::reject_all;
    return std::nullopt;
}

struct DecisionConfig {
    ColdStartPolicy cold_start = ColdStartPolicy::pass_through_all;
    // Retired with PASS_THROUGH_RECOMMENDED: requiring AltStrategy's execution
    // recommendation made an economic verdict the gate of a validity service.
    // Parsed only to refuse `true`.
    bool require_upstream_recommendation = false;
    // Retired with the learned gate (see decide()): a score floor is a learned
    // gate by another name. Parsed only to refuse a non-zero value, so a config
    // that still sets one fails loudly instead of silently doing nothing.
    double min_quality_score = 0.0;
};

enum class DecisionBasis {
    model, cold_start, model_unusable, upstream_not_recommended, malformed_signal,
    score_below_floor,
    // Older than the configured freshness window. It is rejected BEFORE the
    // model is consulted — a stale signal that scores well is still a stale
    // signal — so it must not be reported as "no usable model".
    signal_stale,
    // Declares a holding period beyond the stack's absolute ceiling. Checked
    // here INDEPENDENTLY of AltStrategy: the ceiling is a safety property of
    // the whole system, and a downstream stage that trusts an upstream stage to
    // enforce it has not enforced it.
    holding_exceeds_ceiling
};

[[nodiscard]] inline std::string_view decision_basis_name(DecisionBasis b) noexcept {
    switch (b) {
        case DecisionBasis::model: return "MODEL";
        case DecisionBasis::cold_start: return "COLD_START";
        case DecisionBasis::model_unusable: return "MODEL_UNUSABLE";
        case DecisionBasis::upstream_not_recommended: return "UPSTREAM_NOT_RECOMMENDED";
        case DecisionBasis::malformed_signal: return "MALFORMED_SIGNAL";
        case DecisionBasis::score_below_floor: return "SCORE_BELOW_FLOOR";
        case DecisionBasis::signal_stale: return "SIGNAL_STALE";
        case DecisionBasis::holding_exceeds_ceiling: return "HOLDING_EXCEEDS_CEILING";
    }
    return "?";
}

// The inverse of decision_basis_name, for reading a persisted decision back.
[[nodiscard]] inline std::optional<DecisionBasis> parse_decision_basis(std::string_view s) {
    if (s == "MODEL") return DecisionBasis::model;
    if (s == "COLD_START") return DecisionBasis::cold_start;
    if (s == "MODEL_UNUSABLE") return DecisionBasis::model_unusable;
    if (s == "UPSTREAM_NOT_RECOMMENDED") return DecisionBasis::upstream_not_recommended;
    if (s == "MALFORMED_SIGNAL") return DecisionBasis::malformed_signal;
    if (s == "SCORE_BELOW_FLOOR") return DecisionBasis::score_below_floor;
    if (s == "SIGNAL_STALE") return DecisionBasis::signal_stale;
    if (s == "HOLDING_EXCEEDS_CEILING") return DecisionBasis::holding_exceeds_ceiling;
    return std::nullopt;
}

struct QualityDecision {
    bool accepted = false;
    DecisionBasis basis = DecisionBasis::cold_start;
    std::vector<std::string> reason_codes;
    bool has_score = false;
    double quality_score = 0;
    double threshold = 0;
};

// The one place ACCEPT/REJECT is decided. `scored` is false when there is no
// usable model; `model_unusable` distinguishes "not trained yet" from "trained
// but the artifact does not match the running contract", because those need
// different operator responses.
// The absolute holding ceiling for the whole stack. Declared here as well as in
// AltStrategy on purpose: this is the last stage before delivery that can refuse
// a signal, and an invariant enforced in exactly one place is enforced only as
// long as that place is correct.
inline constexpr int64_t kMaxHoldingMs = 48LL * 3600 * 1000;

[[nodiscard]] inline QualityDecision decide(const DecisionConfig& cfg, bool upstream_recommended,
                                            bool scored, double quality_score, double threshold,
                                            bool model_unusable, bool stale = false,
                                            int64_t max_holding_time_ms = 0) {
    QualityDecision d;
    d.has_score = scored;
    d.quality_score = quality_score;
    d.threshold = threshold;

    // Checked before everything, including staleness: a signal that would hold a
    // position past the ceiling must never be accepted, whatever else is true
    // of it and whatever a model would have scored it.
    if (max_holding_time_ms > kMaxHoldingMs) {
        d.accepted = false;
        d.has_score = false;
        d.basis = DecisionBasis::holding_exceeds_ceiling;
        d.reason_codes.emplace_back("HOLDING_EXCEEDS_CEILING");
        return d;
    }
    if (stale) {
        // Checked first and reported on its own: the signal describes a market
        // that no longer exists, whatever a model would have said about it.
        d.accepted = false;
        d.has_score = false;
        d.basis = DecisionBasis::signal_stale;
        d.reason_codes.emplace_back("SIGNAL_STALE");
        return d;
    }
    // AltStrategy's recommendation is not consulted: VPS1 takes no execution
    // decision, and the flag is always false (see ColdStartPolicy).
    (void)upstream_recommended;
    // Validity decides; a learned score never does.
    //
    // This stage used to accept a signal only when its own model's quality
    // score cleared a fitted threshold: a second, independently trained
    // scoring system sitting after AltML's calibrated economics, able to veto
    // a signal the economic policy recommended and to pass one it had
    // withheld. That is not quality/validity -- it is a second opinion on the
    // edge, trained on a different target and invisible to the decision layer
    // it overrides. The edge is decided once, upstream, by the calibrated
    // estimate against the estimated cost. A score, when a model exists, is
    // recorded beside the verdict as a diagnostic and nothing more.
    //
    // The wire basis for a validity verdict stays COLD_START (the contract is
    // vendored verbatim downstream): "no deciding model" is exactly what it
    // has always meant, and is now permanent.
    d.basis = model_unusable ? DecisionBasis::model_unusable : DecisionBasis::cold_start;
    if (model_unusable) d.reason_codes.emplace_back("MODEL_UNUSABLE");
    switch (cfg.cold_start) {
        case ColdStartPolicy::pass_through_all:
            d.accepted = true;
            d.reason_codes.emplace_back("COLD_START_PASS_THROUGH_ALL");
            break;
        case ColdStartPolicy::reject_all:
            d.accepted = false;
            d.reason_codes.emplace_back("COLD_START_REJECT_ALL");
            break;
    }
    if (scored) d.reason_codes.emplace_back("DIAGNOSTIC_SCORE_NOT_USED");
    (void)quality_score;
    (void)threshold;
    return d;
}

} // namespace altsignalquality::domain
