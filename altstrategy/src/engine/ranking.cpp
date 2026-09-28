#include "altstrategy/engine/ranking.hpp"

#include "altstrategy/engine/cost_model.hpp"
#include "altstrategy/engine/trade_plan.hpp"

#include "altstrategy/engine/ids.hpp"

#include <algorithm>
#include <cmath>

namespace altstrategy::engine {

// EPISTEMIC STATUS OF THESE WEIGHTS: unvalidated assumptions.
//
// They were chosen by hand and have never been tested against outcomes. The
// machinery to test them exists -- `ranking_challenger_runs` and
// `ranking_policy_versions` -- and on the live deployment both tables hold ZERO
// rows, so no champion/challenger comparison has ever run. Calling this policy
// "tuned" or "optimised" would be a claim nobody has earned.
//
// What they ARE: a deterministic, versioned ordering over quantities that each
// have a defined meaning and range. That is enough to rank candidates
// reproducibly and to be replaced later by something measured. It is not
// evidence that this particular ordering is the best one.
//
// The signs, unlike the magnitudes, are not arbitrary: a component that means
// "more edge" carries a positive weight and one that means "more risk or cost"
// carries a negative one. A sign error is a correctness bug -- see
// `ml_conviction` below, which spent its life as `ml_uncertainty` under a
// negative weight while carrying a conviction measure.
//
// Bump `ranking_policy_version` on any change here, and record the version with
// the decisions it produced, so a later measurement can attribute outcomes to
// the ordering that generated them.
// The champion ranks on ONE quantity: the economic policy's expected net
// return for the candidate's side, in basis points, from AltML's calibrated
// econ estimate. The hand-weighted sum below it was measured out of sample on
// 21.9k candidates (Sep 2026, the same rows for both): its rank correlation
// with the realised net outcome was +0.009 / -0.011 / -0.025 at 15m / 1h /
// 4h, and picking its top candidate per context lost 14.8 / 14.1 / 8.7 bps;
// the ML estimate's correlation was +0.038 / +0.044 / +0.018. The weighted
// sum survives as the CHALLENGER, recorded beside every champion ranking so
// the comparison keeps being measured -- it decides nothing.
RankingPolicy default_champion_policy() {
    RankingPolicy p;
    p.ranking_policy_id = "champion_ml_v2";
    p.ranking_policy_version = "2.0.0";
    p.is_champion = true;
    p.weights = {{"ml_expected_net_bps", 1.0}};
    return p;
}

RankingPolicy default_challenger_policy() {
    RankingPolicy p;
    p.ranking_policy_id = "challenger_static_v1";
    p.ranking_policy_version = "1.0.0";
    p.is_champion = false;
    p.weights = {
        {"static_score", 1.0},      {"adaptive_score", 1.2},   {"posterior_edge", 0.9},
        {"ess_confidence", 0.5},    {"family_agreement", 0.6}, {"ml_direction", 0.8},
        {"ml_net_return", 0.7},     {"ml_excursion_rr", 0.4},  {"regime_alignment", 0.5},
        {"cross_venue", 0.4},       {"structure_room", 0.3},   {"fill_probability", 0.3},
        // Four independent technical readings of the same market: agreement is
        // confirmation, disagreement is a reason to rank a candidate lower.
        {"venue_trend_agreement", 0.4},   {"venue_volatility_agreement", 0.2},
        {"posterior_uncertainty", -0.5},
        // `ml_uncertainty` carried AltML's confidence -- |p_up - 0.5| * 2, where
        // HIGH means far from a coin flip -- under a NEGATIVE weight. The score
        // was penalising the model's most confident calls. Renamed to what the
        // number is and given the sign that meaning requires. `ml_ood` is gone:
        // nothing computes an OOD score, so its constant 0 was contributing a
        // fixed non-penalty that looked like a measurement.
        {"ml_conviction", 0.5},
        {"regime_divergence", -0.4},{"cost_penalty", -0.6},    {"warning_penalty", -0.3},
        {"venue_rsi_disagreement", -0.3},
    };
    return p;
}

std::map<std::string, double> ranking_components(const StrategyCandidate& c,
                                                 const DecisionContext& ctx,
                                                 const EnsembleView& ensemble) {
    std::map<std::string, double> k;
    // The champion's only input. Present only when the economic policy
    // actually evaluated the candidate -- a missing model contributes no
    // number at all, and rank() orders such a candidate after every evaluated
    // one and never recommends it.
    if (c.econ.evaluated()) k["ml_expected_net_bps"] = c.econ.expected_net_bps;
    // Diagnostic only: what a SHADOW model said. Not in any policy's weights.
    if (c.econ.state == EconState::model_shadow) k["ml_shadow_expected_net_bps"] = c.econ.expected_net_bps;
    k["static_score"] = c.static_score;
    k["adaptive_score"] = c.adaptive_score;
    k["posterior_edge"] = std::tanh(c.strategy_posterior / 0.004);
    k["ess_confidence"] = c.effective_sample_size / (c.effective_sample_size + 20.0);
    k["posterior_uncertainty"] = std::clamp(c.posterior_uncertainty, 0.0, 1.5);

    // Ensemble family agreement, weighted by independence so a same-family pile
    // does not read as strong confirmation (§11).
    const bool aligned = c.side == ensemble.ensemble_direction;
    k["family_agreement"] = aligned ? ensemble.ensemble_agreement * ensemble.family_independence_score
                                    : -ensemble.ensemble_agreement * 0.5;

    const auto& ml = c.ml_context;
    if (ml.present && !ml.abstain) {
        const double dir = c.side == Side::long_ ? ml.p_up - ml.p_down : ml.p_down - ml.p_up;
        k["ml_direction"] = dir;
        const double net = c.side == Side::long_ ? ml.expected_return : -ml.expected_return;
        k["ml_net_return"] = std::tanh(net / 0.004);
        const double mfe = std::abs(ml.expected_mfe);
        const double mae = std::max(std::abs(ml.expected_mae), 1e-6);
        k["ml_excursion_rr"] = std::tanh((mfe / mae - 1.0));
        // Conviction, not uncertainty. AltML's field is |p_up - 0.5| * 2 -- a
        // transform of the probability, so HIGH means far from a coin flip.
        // The old key was named `ml_uncertainty` and fed the same number, which
        // inverts the meaning wherever the score treats it as a penalty.
        k["ml_conviction"] = std::clamp(ml.conviction, 0.0, 1.0);
    } else {
        k["ml_direction"] = 0;
        k["ml_net_return"] = 0;
        k["ml_excursion_rr"] = 0;
        // Absent or abstained: no conviction, rather than a middling one. There
        // is no `ml_ood` key any more -- nothing computes an OOD score, and a
        // constant 0 there read as "in distribution, all clear".
        k["ml_conviction"] = 0;
    }

    const std::string& gr = ctx.global_regime.regime;
    const bool regime_ok = (c.side == Side::long_ && gr == "TREND_UP") ||
                           (c.side == Side::short_ && gr == "TREND_DOWN") || gr == "BREAKOUT" ||
                           gr == "RANGE";
    k["regime_alignment"] = regime_ok ? ctx.global_regime.confidence : 0.0;
    k["regime_divergence"] = ctx.regime_divergence ? 1.0 : 0.0;
    k["cross_venue"] = std::clamp(c.cross_venue_confirmation, 0.0, 1.0);

    // Technical agreement across the four venues. Each venue computes its own
    // indicators from its own candles, so these are four independent readings of
    // the same market rather than one number repeated: when they line up, the
    // setup is a state of the market and not an artefact of one order book.
    const auto& cv = ctx.cross_venue_features;
    {
        // Do the venues agree on the trend, and does it agree with this side?
        // trend_alignment_agreement is the mean of per-venue EMA stacking in
        // [-1,+1]; signed against the candidate's direction it becomes a
        // confirmation score in [0,1].
        // Both of these are emitted ONLY when the cross-venue vector actually
        // carried them. policy_score() skips a component that is absent from the
        // map, so an unobserved venue relation contributes nothing -- whereas a
        // default would contribute a score. `volatility_agreement` defaulted to
        // 0.5, awarding half a point of "the venues agree about volatility" on
        // every context that had no cross-venue data at all.
        if (cv.has("trend_alignment_agreement")) {
            const double trend = cv.get("trend_alignment_agreement");
            const double signed_trend = c.side == Side::long_ ? trend : -trend;
            k["venue_trend_agreement"] = std::clamp(signed_trend, 0.0, 1.0);
        }

        // Agreement on realized volatility. Venues that disagree about how
        // volatile the instrument is are not describing the same market, and a
        // stop sized from one of them is not safe on the others.
        if (cv.has("volatility_agreement")) {
            k["venue_volatility_agreement"] =
                std::clamp(cv.get("volatility_agreement"), 0.0, 1.0);
        }

        // RSI dispersion penalises disagreement directly. A dispersion of ~10
        // RSI points across venues is already a lot for the same instrument on
        // the same minute, so that is the full penalty.
        if (cv.has("rsi_dispersion")) {
            k["venue_rsi_disagreement"] = std::clamp(cv.get("rsi_dispersion") / 10.0, 0.0, 1.0);
        }
    }

    // Structure room to the opposing zone in ATR units.
    double room = 1.0;
    const std::string opp = c.side == Side::long_ ? "resistance" : "support";
    if (const auto* z = ctx.nearest_zone(opp, ctx.last_price()); z && ctx.atr() > 0)
        room = std::clamp(std::abs(z->center - ctx.last_price()) / ctx.atr() / 3.0, 0.0, 1.0);
    k["structure_room"] = room;

    // Cheap fill-probability proxy: tighter spread + deeper book = higher.
    const double spread_pen = std::clamp(ctx.spread_bps / 10.0, 0.0, 1.0);
    k["fill_probability"] = std::clamp(1.0 - spread_pen, 0.0, 1.0);

    // Round-trip cost drag, from the CANONICAL model rather than a second
    // formula. This computed `2 * fees_bps + spread_bps + expected_slippage_bps`
    // and was wrong twice over: `fees_bps` is a fixed 4.0 that ignores whether
    // the entry is a maker or a taker fill (the real round trip is 8 or 12 bps),
    // and `expected_slippage_bps` already contains half the spread, so the
    // spread was charged one and a half times -- the very error cost_model.hpp
    // documents having fixed on its own side. Ranking priced a cost no plan
    // would ever pay, and priced it identically for a resting limit and a
    // market order.
    const TradeCost rank_cost = compute_trade_cost(choose_entry(c, ctx), ctx);
    k["cost_penalty"] = std::clamp(rank_cost.round_trip_bps / 30.0, 0.0, 1.5);
    k["warning_penalty"] = std::clamp(static_cast<double>(c.warnings.size()) * 0.3, 0.0, 1.5);
    return k;
}

double policy_score(const RankingPolicy& policy, const std::map<std::string, double>& components) {
    double s = 0;
    for (const auto& [name, w] : policy.weights) {
        if (auto it = components.find(name); it != components.end()) s += w * it->second;
    }
    return s;
}

RankedProposal rank(const std::vector<StrategyCandidate>& candidates, const DecisionContext& ctx,
                    const EnsembleView& ensemble, const RankingPolicy& policy, int64_t now_ms,
                    double close_gap) {
    RankedProposal rp;
    rp.canonical_pair_id = ctx.canonical_pair_id;
    rp.context_id = ctx.context_id;
    rp.ranking_policy_id = policy.ranking_policy_id;
    rp.ranking_policy_version = policy.ranking_policy_version;
    rp.created_at_ms = now_ms;
    rp.ensemble = ensemble;
    rp.is_challenger = !policy.is_champion;
    rp.natural_key = ids::join({ids::i64(ctx.canonical_pair_id), ctx.context_id,
                                policy.ranking_policy_id, policy.is_champion ? "champ" : "chall"});
    rp.ranking_id = ids::make("rank", rp.natural_key);

    if (candidates.empty()) {
        rp.result = RankingResult::no_candidates;
        return rp;
    }

    for (const auto& c : candidates) {
        RankedCandidate rc;
        rc.candidate_id = c.candidate_id;
        rc.strategy_id = c.strategy_id;
        rc.strategy_family = c.strategy_family;
        rc.side = c.side;
        rc.adaptive_weight = c.adaptive_score > 0 && c.static_score > 0 ? c.adaptive_score / c.static_score : 1.0;
        rc.posterior_edge = c.strategy_posterior;
        rc.selection_mode = c.selection_mode;
        rc.components = ranking_components(c, ctx, ensemble);
        rc.final_score = policy_score(policy, rc.components);
        rp.ranked_candidates.push_back(std::move(rc));
    }
    // Deterministic order, including ties. Comparing the score alone leaves the
    // relative order of equal scores to std::sort, which is not stable: the same
    // context could rank the same two candidates either way, pick a different
    // `recommended_candidate_id`, and publish a different signal -- with nothing
    // in the record to show why. `candidate_id` is derived from the candidate's
    // natural key, so it is stable across runs and across restarts, which a
    // pointer or an insertion index would not be. No tie has been observed in
    // seven days of production, and that is exactly why this has to be pinned
    // now rather than after two strategies start agreeing.
    // A champion ranking orders economically evaluated candidates first: a
    // candidate with no usable model has no score, not a score of zero, and
    // must not outrank one the model says loses 3 bps merely because 0 > -3.
    std::map<std::string, bool> evaluated;
    std::map<std::string, bool> actionable;
    std::map<std::string, double> shadow_net;   // only for candidates a SHADOW model scored
    for (const auto& c : candidates) {
        evaluated[c.candidate_id] = c.econ.evaluated();
        actionable[c.candidate_id] = c.econ.actionable();
        if (c.econ.state == EconState::model_shadow) shadow_net[c.candidate_id] = c.econ.expected_net_bps;
    }
    const bool econ_policy = policy.is_champion;
    std::sort(rp.ranked_candidates.begin(), rp.ranked_candidates.end(),
              [&](const RankedCandidate& a, const RankedCandidate& b) {
                  if (econ_policy && evaluated[a.candidate_id] != evaluated[b.candidate_id]) {
                      return evaluated[a.candidate_id];
                  }
                  if (econ_policy && !evaluated[a.candidate_id]) {
                      // Among the unevaluated, a SHADOW estimate orders which one
                      // is planned for OBSERVATION; it never recommends.
                      const bool sa = shadow_net.count(a.candidate_id) > 0;
                      const bool sb = shadow_net.count(b.candidate_id) > 0;
                      if (sa != sb) return sa;
                      if (sa && shadow_net[a.candidate_id] != shadow_net[b.candidate_id]) {
                          return shadow_net[a.candidate_id] > shadow_net[b.candidate_id];
                      }
                  }
                  if (a.final_score != b.final_score) return a.final_score > b.final_score;
                  return a.candidate_id < b.candidate_id;
              });
    for (size_t i = 0; i < rp.ranked_candidates.size(); ++i)
        rp.ranked_candidates[i].rank = static_cast<int>(i) + 1;

    const auto& top = rp.ranked_candidates.front();
    // EXPLORE_SHADOW candidates never become the executable recommendation,
    // and under the champion neither does one without a positive economic
    // verdict: the recommendation is FAIL-CLOSED on the model.
    std::string recommended;
    for (const auto& rc : rp.ranked_candidates) {
        if (rc.selection_mode == SelectionMode::explore_shadow) continue;
        if (econ_policy && !actionable[rc.candidate_id]) continue;
        recommended = rc.candidate_id;
        break;
    }
    rp.recommended_candidate_id = recommended;

    if (econ_policy) {
        // The champion's result follows the economics: a recommendation only
        // when there is an actionable candidate at all.
        if (recommended.empty()) {
            rp.result = RankingResult::no_clear_winner;
        } else if (rp.ranked_candidates.size() >= 2 &&
                   rp.ranked_candidates[1].final_score > 0 && top.side != rp.ranked_candidates[1].side &&
                   actionable[rp.ranked_candidates[1].candidate_id]) {
            rp.result = RankingResult::conflicted;   // both sides "pay": trust neither
            rp.recommended_candidate_id.clear();
        } else {
            rp.result = RankingResult::recommended;
        }
    } else if (rp.ranked_candidates.size() == 1) {
        rp.result = top.final_score > 0 ? RankingResult::recommended : RankingResult::no_clear_winner;
    } else {
        const auto& second = rp.ranked_candidates[1];
        const double gap = top.final_score - second.final_score;
        if (top.final_score <= 0) {
            rp.result = RankingResult::no_clear_winner;
        } else if (gap >= close_gap) {
            rp.result = RankingResult::recommended;
        } else if (top.side != second.side) {
            rp.result = RankingResult::conflicted;
        } else {
            rp.result = RankingResult::multiple_close;
        }
    }

    // Ranking uncertainty from the top gap + ensemble disagreement; regret from
    // how close the runner-up (or best conflicting) sits to the pick.
    if (rp.ranked_candidates.size() >= 2) {
        const double gap = rp.ranked_candidates[0].final_score - rp.ranked_candidates[1].final_score;
        rp.ranking_uncertainty = std::clamp(1.0 - std::tanh(std::abs(gap)) , 0.0, 1.0) *
                                 (1.0 - ensemble.ensemble_agreement * 0.5);
        rp.ranking_regret_estimate = std::max(0.0, rp.ranked_candidates[1].final_score) *
                                     rp.ranking_uncertainty;
    } else {
        rp.ranking_uncertainty = 0.3;
    }
    return rp;
}

} // namespace altstrategy::engine
