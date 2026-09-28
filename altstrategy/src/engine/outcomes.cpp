#include "altstrategy/engine/outcomes.hpp"

#include <map>

#include "altstrategy/engine/ids.hpp"

#include <algorithm>
#include <cmath>

namespace altstrategy::engine {

std::vector<CandidateMarketOutcome> candidate_market_outcomes(
    const std::string& candidate_id, Side side, int64_t created_at_ms,
    const std::vector<PathBar>& bars, std::optional<double> cost_bps,
    const std::vector<int32_t>& horizons_minutes) {
    constexpr int64_t kMin = 60'000;
    std::vector<CandidateMarketOutcome> out;
    if (side != Side::long_ && side != Side::short_) return out;
    const int64_t ref_close = (created_at_ms / kMin) * kMin;   // last close at or before creation
    std::map<int64_t, const PathBar*> by_open;
    for (const auto& b : bars) {
        // The market outcome is defined on the 1m grid. A bar not aligned to
        // a minute is from a finer source and would be read as a whole minute.
        if (b.open_ms % kMin != 0) return out;
        by_open[b.open_ms] = &b;
    }
    const auto entry_it = by_open.find(ref_close - kMin);
    if (entry_it == by_open.end() || !(entry_it->second->close > 0)) return out;
    const double entry = entry_it->second->close;
    const double s = side == Side::long_ ? 1.0 : -1.0;
    for (const int32_t h : horizons_minutes) {
        double hi = -1e300, lo = 1e300, exit = 0;
        int32_t k_hi = 0, k_lo = 0;
        bool complete = h > 0;
        for (int32_t k = 0; k < h && complete; ++k) {
            const auto it = by_open.find(ref_close + k * kMin);
            if (it == by_open.end() || !(it->second->high > 0) || !(it->second->low > 0)) {
                complete = false;
                break;
            }
            // The FIRST bar reaching the extreme: when it was first available.
            if (it->second->high > hi) { hi = it->second->high; k_hi = k + 1; }
            if (it->second->low < lo) { lo = it->second->low; k_lo = k + 1; }
            exit = it->second->close;
        }
        if (!complete || !(exit > 0)) continue;
        CandidateMarketOutcome o;
        o.candidate_id = candidate_id;
        o.horizon_minutes = h;
        o.entry_ref_ms = ref_close;
        o.entry_price = entry;
        o.exit_price = exit;
        o.gross_bps = s * (exit / entry - 1.0) * 10'000.0;
        const double up = (hi / entry - 1.0) * 10'000.0, dn = (lo / entry - 1.0) * 10'000.0;
        o.mfe_bps = side == Side::long_ ? up : -dn;
        o.mae_bps = side == Side::long_ ? dn : -up;
        o.t_mfe_min = side == Side::long_ ? k_hi : k_lo;
        o.t_mae_min = side == Side::long_ ? k_lo : k_hi;
        o.cost_bps = cost_bps;
        if (cost_bps) o.net_bps = o.gross_bps - *cost_bps;
        out.push_back(o);
    }
    return out;
}

namespace {

bool touches_long_entry(const PathBar& b, double entry) { return b.low <= entry; }
bool touches_short_entry(const PathBar& b, double entry) { return b.high >= entry; }
bool triggers_long_stop_entry(const PathBar& b, double trig) { return b.high >= trig; }
bool triggers_short_stop_entry(const PathBar& b, double trig) { return b.low <= trig; }

} // namespace

MarketOutcome reconstruct_outcome(const OutcomeGeometry& g, const std::vector<PathBar>& bars,
                                  OutcomePath source, const std::string& candidate_id,
                                  const std::string& signal_proposal_id, int64_t now_ms) {
    MarketOutcome o;
    o.candidate_id = candidate_id;
    o.signal_proposal_id = signal_proposal_id;
    o.path_source = source;
    o.created_at_ms = now_ms;
    o.is_counterfactual = signal_proposal_id.empty();
    o.outcome_id = ids::make("mkt", ids::join({candidate_id, signal_proposal_id.empty() ? "cf" : signal_proposal_id}));

    if (bars.empty()) { o.outcome = MarketOutcomeType::data_incomplete; return o; }
    const bool is_long = g.side == Side::long_;
    // The price the entry ORDER references (limit/stop level). Fill price is
    // resolved separately below, because a market order does not fill here.
    const double entry = g.entry_type == EntryType::stop_market || g.entry_type == EntryType::stop_limit
                             ? g.trigger_price : g.entry_price;

    // ---- entry detection within the entry window ----
    const int64_t entry_deadline = g.created_at_ms + std::max<int64_t>(g.entry_fill_timeout_ms, 60'000);
    int64_t entry_time = 0;
    bool entered = false;
    // A limit/stop entry fills AT its price by construction. A MARKET entry fills
    // at whatever the market is when it is sent, so crediting it with the plan's
    // pre-computed entry_price would silently assume the estimate was exact and
    // the slippage zero — measuring the plan instead of the trade.
    double fill_price = entry;
    for (const auto& b : bars) {
        if (b.open_ms < g.created_at_ms) continue;
        if (b.open_ms > entry_deadline) break;
        bool filled = false;
        if (g.entry_type == EntryType::market) {
            filled = true;
            if (b.open > 0) fill_price = b.open;
        }
        else if (g.entry_type == EntryType::stop_market || g.entry_type == EntryType::stop_limit) {
            filled = is_long ? triggers_long_stop_entry(b, entry) : triggers_short_stop_entry(b, entry);
        } else {
            filled = is_long ? touches_long_entry(b, entry) : touches_short_entry(b, entry);
        }
        if (filled) { entry_time = std::max(b.open_ms, g.created_at_ms); entered = true; break; }
    }
    if (!entered) {
        // Not filled. If the data covers the fill window the entry was simply
        // never reached; otherwise we cannot yet tell (§19: never guess).
        const int64_t last = bars.back().open_ms;
        o.outcome = last >= entry_deadline ? MarketOutcomeType::entry_not_reached
                                           : MarketOutcomeType::data_incomplete;
        return o;
    }
    // A fill that is already past the target or past the stop is not an entry
    // into this plan -- it is the plan being overtaken before it started.
    //
    // A MARKET entry fills at the bar's open, and the SAME bar is then scanned
    // for target/stop. When a gap opens beyond the target, the bar trivially
    // "touches" it and the trade was recorded as TARGET_FIRST while the exit at
    // the target price was WORSE than the fill: measured on VPS1, 2 of 4309
    // TARGET_FIRST outcomes carried a negative gross return (-49.1 and -5.2 bps)
    // and 1 of 7067 STOP_FIRST carried a positive one (+10.6 bps). A label that
    // contradicts its own economics corrupts every statistic built on it -- win
    // rate, expectancy and the posterior that ranks the strategy.
    //
    // Real execution would not have taken this entry. It is recorded as
    // ENTRY_EXPIRED, which is what it is, rather than as a trade that never
    // could have happened.
    const bool fill_past_target =
        is_long ? fill_price >= g.take_profit_price : fill_price <= g.take_profit_price;
    const bool fill_past_stop =
        is_long ? fill_price <= g.stop_loss_price : fill_price >= g.stop_loss_price;
    if (fill_past_target || fill_past_stop) {
        o.outcome = MarketOutcomeType::entry_expired;
        o.entry_time_ms = entry_time;
        o.entry_fill_price = fill_price;
        o.resolved_time_ms = entry_time;
        return o;
    }

    o.entry_time_ms = entry_time;
    o.entry_fill_price = fill_price;

    // ---- post-entry path to target / stop / time-stop ----
    double mfe = 0, mae = 0;   // in return fraction from the ACTUAL fill
    const double dir = is_long ? 1.0 : -1.0;
    for (const auto& b : bars) {
        if (b.open_ms < entry_time) continue;
        if (b.open_ms > g.time_stop_at_ms) break;
        // Track excursions.
        const double fav = is_long ? (b.high - fill_price) / fill_price : (fill_price - b.low) / fill_price;
        const double adv = is_long ? (fill_price - b.low) / fill_price : (b.high - fill_price) / fill_price;
        mfe = std::max(mfe, fav);
        mae = std::max(mae, adv);

        const bool tgt = is_long ? b.high >= g.take_profit_price : b.low <= g.take_profit_price;
        const bool stp = is_long ? b.low <= g.stop_loss_price : b.high >= g.stop_loss_price;
        if (tgt && stp) {
            // Both inside one bar: only orderable with a sub-bar path. At 1m it is
            // AMBIGUOUS (§19); finer sources are assumed pre-ordered by the caller.
            if (source == OutcomePath::m1_candles) {
                o.outcome = MarketOutcomeType::ambiguous;
                o.ambiguous = true;
                o.resolved_time_ms = b.open_ms;
                o.mfe = mfe; o.mae = mae;
                o.holding_time_ms = b.open_ms - entry_time;
                return o;
            }
            // Finer path: decide by which is closer to the bar open (best effort).
            const bool stop_first = std::abs(b.open - g.stop_loss_price) < std::abs(b.open - g.take_profit_price);
            o.outcome = stop_first ? MarketOutcomeType::stop_first : MarketOutcomeType::target_first;
            o.exit_price = stop_first ? g.stop_loss_price : g.take_profit_price;
            o.resolved_time_ms = b.open_ms;
            o.holding_time_ms = b.open_ms - entry_time;
            o.mfe = mfe; o.mae = mae;
            const double gross = dir * (o.exit_price - fill_price) / fill_price;
            o.realized_gross_return = gross;
            o.realized_net_return = gross - g.cost_bps / 10000.0;
            return o;
        }
        if (tgt) {
            o.outcome = MarketOutcomeType::target_first;
            o.exit_price = g.take_profit_price;
        } else if (stp) {
            o.outcome = MarketOutcomeType::stop_first;
            o.exit_price = g.stop_loss_price;
        } else {
            continue;
        }
        o.resolved_time_ms = b.open_ms;
        o.holding_time_ms = b.open_ms - entry_time;
        o.mfe = mfe; o.mae = mae;
        const double gross = dir * (o.exit_price - fill_price) / fill_price;
        o.realized_gross_return = gross;
        o.realized_net_return = gross - g.cost_bps / 10000.0;
        return o;
    }

    // Neither hit before the time-stop → TIME_STOP, exit at last close in window.
    double exit_close = fill_price;
    int64_t last_ms = entry_time;
    for (const auto& b : bars) {
        if (b.open_ms < entry_time || b.open_ms > g.time_stop_at_ms) continue;
        exit_close = b.close;
        last_ms = b.open_ms;
    }
    if (last_ms < g.time_stop_at_ms - 60'000 && bars.back().open_ms < g.time_stop_at_ms) {
        o.outcome = MarketOutcomeType::data_incomplete;
        return o;
    }
    o.outcome = MarketOutcomeType::time_stop;
    o.exit_price = exit_close;
    o.resolved_time_ms = last_ms;
    o.holding_time_ms = last_ms - entry_time;
    o.mfe = mfe; o.mae = mae;
    const double gross = dir * (exit_close - fill_price) / fill_price;
    o.realized_gross_return = gross;
    o.realized_net_return = gross - g.cost_bps / 10000.0;
    return o;
}

std::vector<CounterfactualOutcome> compare_counterfactuals(const std::string& ranking_id,
                                                           const MarketOutcome& selected,
                                                           const std::vector<MarketOutcome>& cfs,
                                                           int64_t now_ms,
                                                           const std::vector<int>& plan_versions) {
    std::vector<CounterfactualOutcome> out;
    // Best achievable net return across selected + counterfactuals (hindsight).
    double best = selected.realized_net_return;
    std::string best_id = selected.candidate_id;
    // Plan variants of one candidate all carry the SAME candidate_id, so the
    // winner has to be identified by position; matching on candidate_id would
    // mark every sibling as the best-in-hindsight one. -1 means the selected
    // outcome was already the best available.
    int best_pos = -1;
    for (size_t i = 0; i < cfs.size(); ++i) {
        const auto& cf = cfs[i];
        if (cf.outcome == MarketOutcomeType::ambiguous || cf.outcome == MarketOutcomeType::data_incomplete)
            continue;
        if (cf.realized_net_return > best) {
            best = cf.realized_net_return;
            best_id = cf.candidate_id;
            best_pos = static_cast<int>(i);
        }
    }
    const bool versioned = plan_versions.size() == cfs.size();
    for (size_t i = 0; i < cfs.size(); ++i) {
        const auto& cf = cfs[i];
        CounterfactualOutcome c;
        c.ranking_id = ranking_id;
        // When the alternatives are plan VARIANTS of one candidate they all share
        // candidate_id, so the variant number is what makes the row identifiable.
        c.plan_version = versioned ? plan_versions[i] : 1;
        c.selected_candidate_id = selected.candidate_id;
        c.candidate_id = cf.candidate_id;
        c.outcome = cf.outcome;
        c.realized_net_return = cf.realized_net_return;
        c.ranking_regret = best - selected.realized_net_return;   // >=0
        c.best_hindsight = versioned ? (static_cast<int>(i) == best_pos)
                                     : (cf.candidate_id == best_id);
        c.created_at_ms = now_ms;
        c.counterfactual_id = ids::make(
            "cf", ids::join({ranking_id, cf.candidate_id,
                             versioned ? std::to_string(c.plan_version) : std::string{}}));
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace altstrategy::engine
