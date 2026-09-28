#pragma once

// Adaptive subsystems (§35, §36, §37, §40):
//   * strategy_drift_monitor  — baseline vs recent metric drift → status.
//   * parameter_calibrator    — bounded stop/target/timeout recalibration from
//                               predicted-vs-actual, staying inside the schema.
//   * lifecycle_coordinator   — versioned status transitions with min samples,
//                               cooldown and rollback (never source/schema).

#include "altstrategy/engine/decision_types.hpp"
#include "altstrategy/engine/strategy.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace altstrategy::engine {

// ------------------------------ drift ---------------------------------------

struct DriftInputs {
    double baseline_candidate_freq = 0, recent_candidate_freq = 0;
    double baseline_win_rate = 0, recent_win_rate = 0;
    double baseline_score_mean = 0, recent_score_mean = 0;
    double baseline_mfe = 0, recent_mfe = 0;
    double baseline_mae = 0, recent_mae = 0;
    double baseline_holding = 0, recent_holding = 0;
    double baseline_fill = 0, recent_fill = 0;
    // Regime and symbol performance are MEAN RETURNS, so a change in them is
    // only interpretable against the spread of the group means it was taken
    // over. Carrying the dispersion and the group count lets the drift measure
    // judge the difference statistically instead of by magnitude alone — see
    // mean_drift() in adapt.cpp for what went wrong without them.
    double baseline_regime_perf = 0, recent_regime_perf = 0;
    double baseline_regime_perf_sd = 0, recent_regime_perf_sd = 0;
    int64_t baseline_regime_groups = 0, recent_regime_groups = 0;
    double baseline_symbol_perf = 0, recent_symbol_perf = 0;
    double baseline_symbol_perf_sd = 0, recent_symbol_perf_sd = 0;
    int64_t baseline_symbol_groups = 0, recent_symbol_groups = 0;
    // Total resolved-and-filled outcomes across BOTH windows.
    int64_t sample_count = 0;
    // ...and split per window. Drift is a COMPARISON, so a verdict needs
    // evidence on both sides of it. Counting only the union let an empty recent
    // window pass the guard on the strength of the baseline alone, and every
    // drift figure was then measured against a win rate of zero that meant
    // "no trades", not "every trade lost".
    int64_t baseline_sample_count = 0;
    int64_t recent_sample_count = 0;
};

[[nodiscard]] StrategyDrift compute_drift(const std::string& strategy_id,
                                          const std::string& strategy_version,
                                          const DriftInputs& in, int64_t now_ms);

// ------------------------------ calibration ---------------------------------

struct CalibrationTargets {
    double predicted_mfe = 0, actual_mfe = 0;
    double predicted_mae = 0, actual_mae = 0;
    double predicted_fill_min = 0, actual_fill_min = 0;
    double predicted_holding_min = 0, actual_holding_min = 0;
    int64_t sample_count = 0;
};

// Returns proposed parameter values, each clamped to its schema bound and moved
// only a bounded fraction toward the observed target (never after every trade).
[[nodiscard]] std::map<std::string, double> calibrate_parameters(
    const std::vector<StrategyParam>& schema, const std::map<std::string, double>& current,
    const CalibrationTargets& t, double learning_rate = 0.3);

// ------------------------------ lifecycle -----------------------------------

struct LifecycleInputs {
    Lifecycle current = Lifecycle::active;
    DriftStatus drift = DriftStatus::stable;
    double posterior_edge = 0;
    double posterior_uncertainty = 1;
    int64_t sample_count = 0;
    int64_t min_samples = 100;
    bool cooldown_elapsed = true;
};

// Returns a transition if warranted (§40): ACTIVE→DEGRADED, DEGRADED→SHADOW,
// SHADOW→CANDIDATE, CANDIDATE→LIMITED, LIMITED→ACTIVE, plus rollback. nullopt to
// stay put. Every transition is versioned, reasoned, min-sampled and cooldowned.
[[nodiscard]] std::optional<LifecycleTransition> evaluate_lifecycle(
    const std::string& strategy_id, const std::string& strategy_version,
    const LifecycleInputs& in, int64_t now_ms);

// The lifecycle a strategy is ACTUALLY in: its override row when one exists,
// otherwise the status it ships as. `strategy_engine.cpp` resolves it exactly
// this way when it grades a candidate, and the maintenance pass must agree --
// the two read the same fact.
//
// Assuming ACTIVE for a missing override instead put 14 of 16 strategies into a
// status they were never in. `evaluate_lifecycle`'s ACTIVE branch only demotes,
// so a strategy shipped as SHADOW or CANDIDATE could never be promoted on
// merit, and its later demotion was written to the audit trail as coming
// "from ACTIVE".
[[nodiscard]] Lifecycle current_lifecycle(const std::string& strategy_id,
                                          const std::map<std::string, Lifecycle>& overrides,
                                          const std::map<std::string, Lifecycle>& declared);

} // namespace altstrategy::engine
