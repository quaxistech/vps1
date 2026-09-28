#pragma once

#include <optional>

// market_outcome_tracker (§18, §19). Reconstructs the outcome of a proposal from
// the best-available price path (raw trades → 1s aggregates → 1s book → 1m
// candles) WITHOUT needing VPS2 execution feedback. When stop and target both
// fall inside one bar at the coarsest (1m) resolution and the order is unknown,
// the outcome is AMBIGUOUS — never silently scored win/loss (§19). Also the
// counterfactual comparison (§20).

#include "altstrategy/engine/decision_types.hpp"

#include <vector>

namespace altstrategy::engine {

// Cost model v1: a flat round-trip assumption, superseded by the decomposed
// fee/spread/slippage model in cost_model.hpp. Retained because outcomes
// written before v2 were scored with it, and pooling them under v2's figure
// would silently restate what those trades cost.
inline constexpr double kLegacyCostModelV1Bps = 8.0;

// One reconstructed path bar, newest-last, covering [open_ms, open_ms+span).
struct PathBar {
    int64_t open_ms = 0;
    double open = 0, high = 0, low = 0, close = 0;
};

// The MARKET outcome of a candidate at one fixed horizon: a position opened at
// the close of the last 1m bar that had closed when the candidate was raised,
// held for `horizon_minutes`, side-adjusted. No fill model, no stop, no
// target: that is what the market did, which is what the economic model was
// asked to predict. The execution line (limit fills, stops, targets) is a
// different record (MarketOutcome, execution feedback).
struct CandidateMarketOutcome {
    std::string candidate_id;
    int32_t horizon_minutes = 0;
    int64_t entry_ref_ms = 0;      // close time of the reference bar
    double entry_price = 0, exit_price = 0;
    double gross_bps = 0, mfe_bps = 0, mae_bps = 0;
    std::optional<double> cost_bps, net_bps;
    // Minutes from the reference close to the bar holding the most
    // favourable / most adverse price (1 = the first bar after the close).
    int32_t t_mfe_min = 0, t_mae_min = 0;
};

// Outcomes for every horizon whose full path is present in `bars`. The bars
// must be 1m candles: anything else (a 1-second trade bucket) is refused, and
// a horizon with any missing minute is left out, not approximated.
[[nodiscard]] std::vector<CandidateMarketOutcome> candidate_market_outcomes(
    const std::string& candidate_id, Side side, int64_t created_at_ms,
    const std::vector<PathBar>& bars, std::optional<double> cost_bps,
    const std::vector<int32_t>& horizons_minutes = {15, 60, 240});

// The geometry the tracker resolves against (mirrors the proposal's price/time
// fields; kept separate so it also serves counterfactual shadow plans).
struct OutcomeGeometry {
    Side side = Side::unspecified;
    EntryType entry_type = EntryType::limit;
    double entry_price = 0;
    double trigger_price = 0;
    double stop_loss_price = 0;
    double take_profit_price = 0;
    int64_t created_at_ms = 0;
    int64_t entry_fill_timeout_ms = 0;
    int64_t valid_until_ms = 0;
    int64_t time_stop_at_ms = 0;
    // The round-trip cost this geometry is scored with. Callers set it from the
    // figure the plan was PRICED with, so an outcome is never scored under a
    // different cost definition than the decision was made under.
    //
    // The default is the legacy cost model v1 flat rate, which is what rows
    // predating v2 were charged -- the same number decision_store.cpp uses when
    // round_trip_cost_bps is NULL. It is named rather than repeated so the two
    // sites cannot drift apart.
    double cost_bps = kLegacyCostModelV1Bps;
};

// Reconstruct the outcome from `bars` (must cover created_at..time_stop). The
// source records which precision tier produced the path (drives ambiguity).
[[nodiscard]] MarketOutcome reconstruct_outcome(const OutcomeGeometry& g,
                                                const std::vector<PathBar>& bars, OutcomePath source,
                                                const std::string& candidate_id,
                                                const std::string& signal_proposal_id, int64_t now_ms);

// Compare a selected outcome against counterfactual candidate outcomes (§20):
// fills ranking_regret (best hindsight net return minus selected) and marks the
// best-hindsight candidate. Returns one CounterfactualOutcome per counterfactual.
[[nodiscard]] std::vector<CounterfactualOutcome> compare_counterfactuals(
    const std::string& ranking_id, const MarketOutcome& selected,
    const std::vector<MarketOutcome>& counterfactuals, int64_t now_ms,
    const std::vector<int>& plan_versions = {});

} // namespace altstrategy::engine
