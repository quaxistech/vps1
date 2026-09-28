#pragma once

// The point-in-time input of one quality evaluation.
//
// EVERY field here is knowable at T0 = the candidate's `created_at_ms`. That is
// the single invariant this whole service rests on, so the struct is
// deliberately plain data with no I/O and no clock: whoever fills it (the live
// evaluator from a NATS message + a context row, or the dataset builder from
// AltStrategy's point-in-time archive) is responsible for reading only sources
// that existed at T0, and `domain/leakage.hpp` asserts the parts that can be
// checked mechanically.
//
// Optionality is explicit. A value that was not observed is `std::nullopt` and
// becomes a MISSING feature (NaN + cleared validity bit) — never a zero, never
// a mean, never the last seen value.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace altsignalquality::domain {

// The candidate TradeSignalProposal, flattened. Mirrors altstrategy.v1 field
// for field; the proto is converted here so the pure layer never links protobuf.
struct CandidateSignal {
    std::string signal_proposal_id;
    std::string candidate_id;
    std::string ranking_id;
    int64_t canonical_pair_id = 0;
    int64_t bitget_instrument_id = 0;
    std::string symbol;
    std::string strategy_id;
    std::string strategy_version;
    std::string parameter_set_id;
    int side = 0;                       // altstrategy.v1.Side
    std::string proposal_grade;         // STRONG | NORMAL | WEAK | EXPERIMENTAL | SHADOW
    int32_t plan_version = 1;
    int holding_class = 0;              // altstrategy.v1.HoldingClass
    std::string quality;                // ContextQuality name
    int64_t created_at_ms = 0;          // T0
    int64_t valid_until_ms = 0;

    int entry_type = 0;
    double entry_price = 0;
    double trigger_price = 0;
    double max_slippage_bps = 0;
    bool post_only_allowed = false;
    double expected_fill_time_p50_min = 0;
    double expected_fill_time_p90_min = 0;
    int64_t entry_fill_timeout_ms = 0;

    double stop_loss_price = 0;
    double stop_distance_bps = 0;
    double stop_distance_atr = 0;
    double take_profit_price = 0;
    double target_distance_bps = 0;

    int64_t expected_holding_time_p50_ms = 0;
    int64_t expected_holding_time_p90_ms = 0;
    int64_t max_holding_time_ms = 0;
    int64_t time_stop_at_ms = 0;

    // Forward-looking economics, valid only when edge_estimated is true.
    // AltStrategy writes NULL for these without a model; the eight fields that
    // used to sit alongside them here had no producer at all and were removed
    // in sq-core:3.
    bool edge_estimated = false;
    // Present only when the serving model PRODUCES it. The econ models (AltML
    // schema 2) estimate a fixed-horizon market outcome and do not compute a
    // target-before-stop probability, so an edge can be estimated while this
    // stays absent -- edge_estimated alone no longer implies it.
    std::optional<double> p_target_before_stop;
    double ml_conviction = 0;
    double expected_net_return = 0;
    double expected_rr = 0;

    bool ml_abstain = false;
    double strategy_confidence = 0;
    double ensemble_agreement = 0;
    double adaptive_score = 0;

    int32_t risk_flag_count = 0;
    int32_t warning_count = 0;
    int32_t reason_code_count = 0;
    int32_t alternative_plan_count = 0;
    bool recommended_for_execution = false;

    std::string context_id;
    int64_t context_revision = 0;
    std::string feature_manifest_hash;
};

// AltStrategy's immutable point-in-time DecisionContext row
// (altstrategy.decision_context_snapshots), looked up by `context_id`. Absent
// when the row has not been written or has aged out of retention; the whole
// context block then becomes MISSING rather than being guessed.
struct ContextSnapshot {
    bool present = false;
    int64_t event_time_ms = 0;
    int64_t available_at_ms = 0;
    std::string data_quality;
    std::string bitget_regime;
    std::string global_regime;
    uint32_t venue_mask = 0;
    uint32_t missing_venue_mask = 0;
    bool regime_divergence = false;
    std::optional<double> fees_bps;
    std::optional<double> funding_rate;
    std::optional<double> mark_price;
    std::optional<double> spread_bps;
    std::optional<double> top_depth_usdt;
    std::optional<double> liquidity_rank;
    std::optional<double> expected_slippage_bps;

    // The named point-in-time analytics AltStrategy fed its own strategies,
    // persisted in decision_context_snapshots.snapshot. Absent on deployments
    // whose AltStrategy does not persist it; every dependent feature is then
    // MISSING and the model simply has no split on it.
    std::map<std::string, double> bitget_features;
    std::map<std::string, double> microstructure;
    std::map<std::string, double> cross_venue;
    // The shortest higher timeframe AltStrategy admitted for this decision, and
    // its analytics. Empty when no higher grid had a closed bar at T0.
    std::map<std::string, double> htf_features;
    int32_t htf_minutes = 0;
    std::optional<double> regime_confidence;
    std::optional<double> regime_trend_strength;
    std::optional<double> regime_volatility;
    std::optional<double> regime_liquidity_quality;

    [[nodiscard]] std::optional<double> lookup(const std::map<std::string, double>& m,
                                               const std::string& key) const {
        const auto it = m.find(key);
        if (it == m.end()) return std::nullopt;
        return it->second;
    }
};

// One shrunk historical aggregate ("how has this cell behaved so far?"). Every
// value is computed only from outcomes that had already matured at T0.
struct PriorCell {
    bool present = false;
    double win_rate = 0;        // shrunk toward the global win rate
    double expectancy = 0;      // shrunk toward the global expectancy
    double effective_samples = 0;
};

struct PriorSnapshot {
    PriorCell global;
    PriorCell strategy;
    PriorCell pair;
    PriorCell regime;
    PriorCell strategy_regime;
    PriorCell strategy_side;
    PriorCell grade;
    PriorCell decayed_strategy;   // time-decayed view of the same strategy cell
};

// Everything one evaluation reads, and nothing else.
struct SignalSnapshot {
    CandidateSignal candidate;
    ContextSnapshot context;
    PriorSnapshot priors;
};

} // namespace altsignalquality::domain
