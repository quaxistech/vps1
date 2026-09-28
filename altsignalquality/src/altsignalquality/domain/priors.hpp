#pragma once

// As-of historical behaviour: "how has this strategy / pair / regime done so
// far?" — computed from outcomes that had ALREADY MATURED at the moment the
// signal was created.
//
// This is the one feature block where leakage is easy and fatal. A signal
// created at T0 may only see outcomes whose maturation time is <= T0. The
// signal's OWN outcome matures later by construction, so it can never be in its
// own prior; but neither may any other signal that was still open at T0, even
// though its row exists in the database today.
//
// The mechanism is an event sweep, not a query filter: maturation events and
// read events are merged in time order and the accumulator is advanced to T0
// before each read. That produces, for every historical row, exactly the
// numbers the live service would have produced at the time — which is what
// makes the training features and the serving features the same quantity.
//
// Live serving uses the same accumulator, advanced to "every outcome matured so
// far", which for a signal arriving now is the same condition.
//
// Shrinkage follows AltStrategy's adaptive allocator convention: a cell's
// estimate is pulled toward the global estimate with a pseudo-count, so a cell
// with three observations reports the global picture rather than a number that
// looks like evidence.

#include "altsignalquality/domain/signal_snapshot.hpp"
#include "altsignalquality/domain/vocabulary.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace altsignalquality::domain {

// One matured outcome, reduced to what the priors need.
struct MaturedOutcome {
    int64_t matured_at_ms = 0;
    // The signal this outcome belongs to. Carried purely so the sort below has
    // a TOTAL order: without it, two outcomes maturing in the same millisecond
    // for the same strategy compare equal, std::sort is not stable, and the
    // decayed statistics of a backfill depend on the library's partitioning —
    // the same dataset would produce a different model on a different run.
    std::string signal_proposal_id;
    std::string strategy_id;
    int64_t canonical_pair_id = 0;
    std::string regime;
    std::string grade;
    int side = 0;
    int label = 0;               // 1 = positive net outcome
    double net_return = 0;
};

// The identity of one aggregation cell. Kept as (kind, key) strings so the
// in-memory accumulator and the altsignalquality.outcome_priors table are the
// same address space.
enum class PriorScope : uint8_t {
    global, strategy, pair, regime, strategy_regime, strategy_side, grade
};

[[nodiscard]] constexpr std::string_view prior_scope_name(PriorScope s) noexcept {
    switch (s) {
        case PriorScope::global: return "GLOBAL";
        case PriorScope::strategy: return "STRATEGY";
        case PriorScope::pair: return "PAIR";
        case PriorScope::regime: return "REGIME";
        case PriorScope::strategy_regime: return "STRATEGY_REGIME";
        case PriorScope::strategy_side: return "STRATEGY_SIDE";
        case PriorScope::grade: return "GRADE";
    }
    return "?";
}

struct PriorCounters {
    int64_t sample_count = 0;
    int64_t win_count = 0;
    double sum_net_return = 0;
    // The time-decayed view. `decay_anchor_ms` is the instant these three are
    // expressed at; advancing the anchor multiplies them by the half-life
    // factor, so the decayed statistics of a cell that stopped trading fade
    // without needing a sweep over every cell on every event.
    double decayed_count = 0;
    double decayed_wins = 0;
    double decayed_return = 0;
    int64_t decay_anchor_ms = 0;
    int64_t last_matured_at_ms = 0;
};

struct PriorConfig {
    // Pseudo-count k: the sample size at which a cell's own estimate and the
    // global estimate carry equal weight. 20 is AltStrategy's own
    // `adaptive_allocator.prior_strength`, and the ESS at which its shrinkage
    // weight is exactly one half — the same bar its execution veto uses.
    double prior_strength = 20.0;
    // Exponential half-life for the decayed view, in days. Also AltStrategy's
    // default: an observation older than ~3 half-lives contributes <15%.
    double half_life_days = 30.0;
};

// Accumulator over matured outcomes. Deterministic: the same outcomes applied
// in the same time order always produce the same counters, on any host.
class PriorAccumulator {
public:
    explicit PriorAccumulator(PriorConfig cfg = {}) : cfg_(cfg) {}

    // Fold one matured outcome in. Callers apply outcomes in non-decreasing
    // `matured_at_ms` order (sweep_as_of and the persisted refresher both sort
    // first); an out-of-order event still lands in the undecayed counters
    // correctly and is never allowed to move a cell's anchor backwards.
    void apply(const MaturedOutcome& o) {
        for (const auto& [scope, key] : scopes_of(o)) {
            auto& c = cells_[cell_key(scope, key)];
            decay_to(c, o.matured_at_ms);
            c.sample_count += 1;
            c.win_count += o.label;
            c.sum_net_return += o.net_return;
            c.decayed_count += 1.0;
            c.decayed_wins += static_cast<double>(o.label);
            c.decayed_return += o.net_return;
            c.last_matured_at_ms = std::max(c.last_matured_at_ms, o.matured_at_ms);
        }
        anchor_ms_ = std::max(anchor_ms_, o.matured_at_ms);
    }

    // Read the shrunk snapshot a signal with this identity, created at
    // `at_ms`, would have seen. Reading never mutates state: the decay is
    // applied to a copy, so two reads at different times of the same cell are
    // both correct and neither disturbs the other.
    [[nodiscard]] PriorSnapshot snapshot(int64_t at_ms, const std::string& strategy_id,
                                         int64_t canonical_pair_id,
                                         const std::string& regime,
                                         const std::string& grade,
                                         int side) const {
        const PriorCounters* g = find(PriorScope::global, "*");
        PriorSnapshot out;
        if (g == nullptr || g->sample_count == 0) return out;  // nothing known yet

        const double global_rate =
            static_cast<double>(g->win_count) / static_cast<double>(g->sample_count);
        const double global_exp = g->sum_net_return / static_cast<double>(g->sample_count);

        out.global.present = true;
        out.global.win_rate = global_rate;
        out.global.expectancy = global_exp;
        out.global.effective_samples = static_cast<double>(g->sample_count);

        out.strategy = shrink(find(PriorScope::strategy, strategy_id), global_rate, global_exp);
        out.pair = shrink(find(PriorScope::pair, std::to_string(canonical_pair_id)),
                          global_rate, global_exp);
        out.regime = shrink(find(PriorScope::regime, regime), global_rate, global_exp);
        out.strategy_regime = shrink(find(PriorScope::strategy_regime, strategy_id + "|" + regime),
                                     global_rate, global_exp);
        out.strategy_side =
            shrink(find(PriorScope::strategy_side,
                        strategy_id + "|" + std::string(side_name(side))),
                   global_rate, global_exp);
        out.grade = shrink(find(PriorScope::grade, grade), global_rate, global_exp);
        out.decayed_strategy = shrink_decayed(find(PriorScope::strategy, strategy_id), at_ms,
                                              global_rate, global_exp);
        return out;
    }

    [[nodiscard]] const std::map<std::string, PriorCounters>& cells() const { return cells_; }
    [[nodiscard]] int64_t anchor_ms() const { return anchor_ms_; }
    [[nodiscard]] const PriorConfig& config() const { return cfg_; }

    // Load persisted counters: the live path restores state instead of
    // replaying the whole outcome history on every start.
    void load_cell(PriorScope scope, const std::string& key, const PriorCounters& c) {
        cells_[cell_key(scope, key)] = c;
        anchor_ms_ = std::max(anchor_ms_, c.last_matured_at_ms);
    }

    [[nodiscard]] static std::string cell_key(PriorScope scope, const std::string& key) {
        return std::string(prior_scope_name(scope)) + "\x1f" + key;
    }

    // Split a persisted key back into its scope name and cell key.
    [[nodiscard]] static std::pair<std::string, std::string> split_key(const std::string& k) {
        const auto sep = k.find('\x1f');
        if (sep == std::string::npos) return {k, {}};
        return {k.substr(0, sep), k.substr(sep + 1)};
    }

    // The (scope, key) pairs one outcome contributes to. Shared by the
    // in-memory sweep and the persisted refresher so the two provably agree.
    [[nodiscard]] static std::vector<std::pair<PriorScope, std::string>> scopes_of(
        const MaturedOutcome& o) {
        return {{PriorScope::global, "*"},
                {PriorScope::strategy, o.strategy_id},
                {PriorScope::pair, std::to_string(o.canonical_pair_id)},
                {PriorScope::regime, o.regime},
                {PriorScope::strategy_regime, o.strategy_id + "|" + o.regime},
                {PriorScope::strategy_side, o.strategy_id + "|" + std::string(side_name(o.side))},
                {PriorScope::grade, o.grade}};
    }

private:
    [[nodiscard]] const PriorCounters* find(PriorScope scope, const std::string& key) const {
        const auto it = cells_.find(cell_key(scope, key));
        return it == cells_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] double decay_factor(int64_t from_ms, int64_t to_ms) const {
        if (cfg_.half_life_days <= 0.0 || to_ms <= from_ms || from_ms == 0) return 1.0;
        const double age_days = static_cast<double>(to_ms - from_ms) / 86'400'000.0;
        return std::pow(0.5, age_days / cfg_.half_life_days);
    }

    void decay_to(PriorCounters& c, int64_t to_ms) const {
        if (c.decay_anchor_ms == 0) {
            c.decay_anchor_ms = to_ms;
            return;
        }
        const double f = decay_factor(c.decay_anchor_ms, to_ms);
        if (f == 1.0) return;
        c.decayed_count *= f;
        c.decayed_wins *= f;
        c.decayed_return *= f;
        c.decay_anchor_ms = to_ms;
    }

    [[nodiscard]] PriorCell shrink(const PriorCounters* c, double global_rate,
                                   double global_exp) const {
        PriorCell out;
        if (c == nullptr || c->sample_count == 0) {
            // No observations for this cell: the honest answer is the global
            // picture with zero own evidence. Reporting "absent" instead would
            // make a brand-new strategy indistinguishable from a failed lookup.
            out.present = true;
            out.win_rate = global_rate;
            out.expectancy = global_exp;
            out.effective_samples = 0.0;
            return out;
        }
        const double n = static_cast<double>(c->sample_count);
        const double w = n / (n + cfg_.prior_strength);
        out.present = true;
        out.win_rate = w * (static_cast<double>(c->win_count) / n) + (1.0 - w) * global_rate;
        out.expectancy = w * (c->sum_net_return / n) + (1.0 - w) * global_exp;
        out.effective_samples = n;
        return out;
    }

    [[nodiscard]] PriorCell shrink_decayed(const PriorCounters* c, int64_t at_ms,
                                           double global_rate, double global_exp) const {
        PriorCell out;
        PriorCounters decayed;
        if (c != nullptr) {
            decayed = *c;
            decay_to(decayed, at_ms);
        }
        if (c == nullptr || decayed.decayed_count <= 0.0) {
            out.present = true;
            out.win_rate = global_rate;
            out.expectancy = global_exp;
            out.effective_samples = 0.0;
            return out;
        }
        const double n = decayed.decayed_count;
        const double w = n / (n + cfg_.prior_strength);
        out.present = true;
        out.win_rate = w * (decayed.decayed_wins / n) + (1.0 - w) * global_rate;
        out.expectancy = w * (decayed.decayed_return / n) + (1.0 - w) * global_exp;
        out.effective_samples = n;
        return out;
    }

    PriorConfig cfg_;
    std::map<std::string, PriorCounters> cells_;
    int64_t anchor_ms_ = 0;
};

// One dataset row's identity, for the as-of sweep.
struct PriorReadRequest {
    int64_t observation_time_ms = 0;
    std::string strategy_id;
    int64_t canonical_pair_id = 0;
    std::string regime;
    std::string grade;
    int side = 0;
};

// Merge maturation events and reads in time order, so every read sees exactly
// the outcomes that had matured by its observation time. Ties are resolved in
// favour of the maturation (an outcome that matured AT T0 was knowable at T0).
//
// Returns one snapshot per request, in the order the requests were given.
[[nodiscard]] inline std::vector<PriorSnapshot> sweep_as_of(
    std::vector<MaturedOutcome> outcomes, const std::vector<PriorReadRequest>& reads,
    PriorConfig cfg = {}) {
    // A total order, so the fold sequence is identical on every run and host.
    std::sort(outcomes.begin(), outcomes.end(), [](const auto& a, const auto& b) {
        if (a.matured_at_ms != b.matured_at_ms) return a.matured_at_ms < b.matured_at_ms;
        return a.signal_proposal_id < b.signal_proposal_id;
    });
    std::vector<size_t> order(reads.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    // stable_sort keeps the caller's order within one timestamp, and reads at
    // the same instant see the same accumulator state anyway, so the result is
    // independent of how the caller ordered them.
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return reads[a].observation_time_ms < reads[b].observation_time_ms;
    });

    PriorAccumulator acc(cfg);
    std::vector<PriorSnapshot> out(reads.size());
    size_t next_outcome = 0;
    for (const size_t idx : order) {
        const auto& r = reads[idx];
        while (next_outcome < outcomes.size() &&
               outcomes[next_outcome].matured_at_ms <= r.observation_time_ms) {
            acc.apply(outcomes[next_outcome]);
            ++next_outcome;
        }
        out[idx] = acc.snapshot(r.observation_time_ms, r.strategy_id, r.canonical_pair_id,
                                r.regime, r.grade, r.side);
    }
    return out;
}

} // namespace altsignalquality::domain
