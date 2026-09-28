#pragma once

// strategy_engine (§14): over one DecisionContext it determines the eligible
// strategies from metadata, runs every one independently, and returns EVERY
// valid candidate — it never picks a winner and never hides a weak setup. The
// candidate carries deterministic reason codes and complete input lineage
// (context id + revision, strategy version, parameter set).

#include "altstrategy/engine/decision_types.hpp"
#include "altstrategy/engine/economic_policy.hpp"
#include "altstrategy/engine/strategy.hpp"

#include <functional>
#include <map>
#include <tuple>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

namespace altstrategy::engine {

struct StrategyEngineConfig {
    double min_rr = 0.0;                 // reject inverted/degenerate geometry only
    int64_t candidate_ttl_ms = 15LL * 60 * 1000;
    EconomicPolicyConfig econ;
};

// Per-strategy runtime state supplied by the lifecycle_coordinator / DB. Absent
// entries fall back to the strategy's default lifecycle + default parameters.
struct CatalogueState {
    std::map<std::string, Lifecycle> lifecycle;      // strategy_id -> status
    std::map<std::string, ParamValues> params;       // strategy_id -> active set
    std::map<std::string, SelectionMode> selection;  // strategy_id -> EXPLOIT/EXPLORE_SHADOW
};

// Where each strategy's contexts went. Diagnostics only -- never a decision
// input. Without it a strategy that is ineligible on every context, one whose
// setup never occurs and one that is never called at all all look the same
// from outside: no candidate. eligible() computed a reason and run() dropped it.
struct StrategyFunnel {
    struct Counts {
        int64_t contexts = 0;                        // run() offered it a context
        std::map<std::string, int64_t> ineligible;   // eligible() reason -> count
        int64_t evaluated = 0;                       // evaluate() was called
        int64_t no_setup = 0;                        // ... and found nothing
        int64_t geometry_rejected = 0;               // no side, or R:R below min_rr
        int64_t horizon_refused = 0;                 // a thesis longer than 48h
        // The same hypothesis (pair, setup, side) again inside the horizon of
        // the one already published: a persisting state, not a new event.
        int64_t repeat_within_horizon = 0;
        int64_t candidates = 0;
        // What the economic policy said about those candidates: a setup the
        // model says does not pay and a setup with no usable model are
        // different findings (POSITIVE_EDGE, NO_EDGE, ML_MISSING, ...).
        std::map<std::string, int64_t> econ;
    };
    std::map<std::string, Counts> by_strategy;

    void merge(const StrategyFunnel& other);
    [[nodiscard]] bool empty() const { return by_strategy.empty(); }
};

// One open hypothesis per (pair, setup, side). A VPS1 signal states a
// hypothesis over its own horizon; a detector whose condition PERSISTS (a
// trend, a price on one side of the session VWAP, a lopsided book) would
// otherwise restate the same hypothesis on every decision bar -- 214 of 621
// contexts in six hours for momentum_continuation alone. Re-issuing it inside
// its horizon adds no information, so it is admitted again only once that
// horizon has elapsed. This is de-duplication of meaning, not a filter on
// quality: the state itself is recorded every minute by AltML.
class HypothesisBook {
public:
    // true (and remembered) when no same-(pair, setup, side) hypothesis is open
    // at `event_ms`; false while one published at t is open, t + horizon > event.
    bool admit(int64_t pair, const std::string& strategy_id, Side side, int64_t event_ms, int64_t horizon_ms) {
        std::lock_guard<std::mutex> lk(mu_);
        const auto key = std::make_tuple(pair, strategy_id, static_cast<int>(side));
        const auto it = open_until_.find(key);
        if (it != open_until_.end() && event_ms < it->second) return false;
        open_until_[key] = event_ms + horizon_ms;
        return true;
    }

private:
    std::mutex mu_;
    std::map<std::tuple<int64_t, std::string, int>, int64_t> open_until_;
};

class StrategyEngine {
public:
    explicit StrategyEngine(StrategyEngineConfig cfg = {});

    // Runs the catalogue over ctx at wall-clock now_ms. Returns all valid
    // candidates (may be empty). Deterministic in (ctx, state).
    // `funnel`, when given, is incremented with where each strategy stopped.
    // `admit(strategy_id, side, horizon_ms)`, when given, is asked once per
    // setup that fired: false means the same hypothesis is still open (see
    // HypothesisBook) and the setup is counted as repeat_within_horizon.
    using Admit = std::function<bool(const std::string&, Side, int64_t)>;
    [[nodiscard]] std::vector<StrategyCandidate> run(const DecisionContext& ctx, int64_t now_ms,
                                                     const CatalogueState& state,
                                                     StrategyFunnel* funnel = nullptr,
                                                     const Admit& admit = {}) const;

    [[nodiscard]] std::vector<StrategyMeta> metas() const;
    [[nodiscard]] size_t size() const { return strategies_.size(); }

    // Eligibility predicate exposed for tests + diagnostics.
    [[nodiscard]] static bool eligible(const StrategyMeta& m, Lifecycle status,
                                       const DecisionContext& ctx, std::string* reject_reason = nullptr);


private:
    StrategyEngineConfig cfg_;
    std::vector<std::unique_ptr<IStrategy>> strategies_;
};

} // namespace altstrategy::engine
