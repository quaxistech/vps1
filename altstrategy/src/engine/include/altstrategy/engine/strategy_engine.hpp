#pragma once

// strategy_engine (§14): over one DecisionContext it determines the eligible
// strategies from metadata, runs every one independently, and returns EVERY
// valid candidate — it never picks a winner and never hides a weak setup. The
// candidate carries deterministic reason codes and complete input lineage
// (context id + revision, strategy version, parameter set).

#include "altstrategy/engine/decision_types.hpp"
#include "altstrategy/engine/economic_policy.hpp"
#include "altstrategy/engine/strategy.hpp"

#include <algorithm>
#include <array>
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
        // DESIGN-04 shadow measurement: how far price had retraced against
        // the hypothesis since it was opened when the setup fired again, in
        // ATR -- [0, 0.5), [0.5, 1), [1, 2), >= 2. Counted for repeats that
        // were suppressed and for re-admissions after the horizon expired. A
        // repeat after a deep reset is a candidate independent episode; one
        // with none is the same state persisting. Gates nothing.
        std::array<int64_t, 4> repeat_by_reset_atr{};
        std::array<int64_t, 4> readmitted_by_reset_atr{};
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
//
// The book is restored from the archive at startup (restore()): it used to be
// memory only, so every restart re-issued every open hypothesis as new.
//
// It also measures (DESIGN-04, shadow) how far price retraced against each
// hypothesis after it opened: the depth of the reset before the setup fired
// again, the evidence an episode rule would need. It changes no admission.
class HypothesisBook {
public:
    struct Admission {
        bool admitted = false;
        // The retracement against the previous hypothesis of this key since it
        // opened, in its ATR; negative when there was none to measure.
        double reset_atr = -1;
    };

    // Price seen on a context of `pair`: the adverse extreme of every
    // hypothesis of the pair is updated.
    void observe(int64_t pair, double price) {
        if (!(price > 0)) return;
        std::lock_guard<std::mutex> lk(mu_);
        const auto lo = book_.lower_bound(std::make_tuple(pair, std::string(), 0));
        for (auto it = lo; it != book_.end() && std::get<0>(it->first) == pair; ++it) {
            const bool is_long = std::get<2>(it->first) == static_cast<int>(Side::long_);
            Entry& e = it->second;
            e.adverse = std::max(e.adverse, is_long ? e.ref_price - price : price - e.ref_price);
        }
    }

    // admitted (and remembered) when no same-(pair, setup, side) hypothesis is
    // open at `event_ms`; not admitted while one opened at t is open,
    // t + horizon > event.
    Admission admit(int64_t pair, const std::string& strategy_id, Side side, int64_t event_ms,
                    int64_t horizon_ms, double price, double atr) {
        std::lock_guard<std::mutex> lk(mu_);
        const auto key = std::make_tuple(pair, strategy_id, static_cast<int>(side));
        Admission a;
        const auto it = book_.find(key);
        if (it != book_.end() && it->second.atr > 0) a.reset_atr = std::max(it->second.adverse, 0.0) / it->second.atr;
        if (it != book_.end() && event_ms < it->second.open_until_ms) return a;
        book_[key] = Entry{event_ms + horizon_ms, price, atr, 0};
        a.admitted = true;
        return a;
    }

    // A hypothesis the archive says is still open (startup).
    void restore(int64_t pair, const std::string& strategy_id, Side side, int64_t open_until_ms,
                 double ref_price, double atr) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& e = book_[std::make_tuple(pair, strategy_id, static_cast<int>(side))];
        if (open_until_ms > e.open_until_ms) e = Entry{open_until_ms, ref_price, atr, 0};
    }

private:
    struct Entry {
        int64_t open_until_ms = 0;
        double ref_price = 0, atr = 0;
        double adverse = 0;   // price units, against the hypothesis' side
    };
    std::mutex mu_;
    std::map<std::tuple<int64_t, std::string, int>, Entry> book_;
};

// The bucket of a reset depth in ATR for the funnel's shadow counters:
// [0, 0.5) -> 0, [0.5, 1) -> 1, [1, 2) -> 2, >= 2 -> 3.
[[nodiscard]] inline size_t reset_bucket(double reset_atr) {
    return reset_atr < 0.5 ? 0 : reset_atr < 1.0 ? 1 : reset_atr < 2.0 ? 2 : 3;
}

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
