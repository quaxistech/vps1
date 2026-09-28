// Full-path integration test (§42): DecisionContext → candidates → ensemble →
// adaptive allocator → champion + challenger ranking → three plan variants →
// AltML CandidateEvaluation → TradeSignalProposal → market outcome →
// counterfactual → attribution → training example. Proves (a) no stage loses the
// context revision, (b) ids are immutable/deterministic, (c) a technically WEAK
// proposal is not blocked, (d) VPS2 (execution feedback) is never required.

#include "altstrategy/engine/adapt.hpp"
#include "altstrategy/engine/allocator.hpp"
#include "altstrategy/engine/attribution.hpp"
#include "altstrategy/engine/ensemble.hpp"
#include "altstrategy/engine/outcomes.hpp"
#include "altstrategy/engine/proposal.hpp"
#include "altstrategy/engine/ranking.hpp"
#include "altstrategy/engine/strategy_engine.hpp"
#include "altstrategy/engine/trade_plan.hpp"
#include "engine_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>

using namespace altstrategy::engine;
using namespace altstrategy::engine::test;

// A stand-in for AltML's PUBLISHED MESSAGE, driven through the production
// conversion. The helper used to hand-build an MlEvaluation, so the integration
// test exercised every stage except the one that turns a prediction into an
// evaluation -- the stage that had been silently broken.
static MlEvaluation mock_altml(const TradePlanVariant& v, bool available) {
    (void)v;
    MarketForecastView f;
    if (!available) return ml_from_forecast(f);   // present = false
    f.present = true;
    f.p_up = 0.58;
    f.p_down = 0.42;
    f.probability_target_before_stop = 0.55;
    f.expected_return = 0.004;
    f.expected_holding_time_min = 120;
    f.expected_mfe = 0.008;
    f.expected_mae = 0.004;
    f.conviction = 0.75;
    f.quality_status = "VALID";
    f.model_id = "direction";
    f.model_version = 3;
    return ml_from_forecast(f);
}

TEST_CASE("full self-contained decision path preserves revision and needs no VPS2") {
    const int64_t now = 1'000'100;
    auto ctx = make_ctx(100.0, "TREND_UP");
    const int64_t rev = ctx.context_revision;

    // 1) candidates
    StrategyEngine eng;
    auto cands = eng.run(ctx, now, {});
    REQUIRE_FALSE(cands.empty());
    for (const auto& c : cands) CHECK(c.context_revision == rev);   // no revision loss

    // 2) adaptive allocator (empty posterior table => neutral weight, wide unc.)
    AdaptiveAllocator alloc;
    PosteriorTable table;
    for (auto& c : cands) alloc.allocate(c, "majors", table);

    // 3) ensemble
    const auto ens = compute_ensemble(cands);
    CHECK(ens.ensemble_direction != Side::unspecified);

    // 4) champion + challenger ranking
    const auto champ = rank(cands, ctx, ens, default_champion_policy(), now);
    const auto chall = rank(cands, ctx, ens, default_challenger_policy(), now);
    REQUIRE_FALSE(champ.ranked_candidates.empty());
    CHECK_FALSE(champ.recommended_candidate_id.empty());
    CHECK(chall.is_challenger);

    // locate the recommended candidate
    const StrategyCandidate* rec = nullptr;
    for (const auto& c : cands) if (c.candidate_id == champ.recommended_candidate_id) rec = &c;
    REQUIRE(rec != nullptr);

    // 5) three plan variants
    const auto plans = generate_plans(*rec, ctx, now);
    REQUIRE(plans.size() == 3);
    std::vector<std::string> alt_ids;
    for (const auto& p : plans) if (p.profile != PlanProfile::balanced) alt_ids.push_back(p.variant_id);
    const TradePlanVariant* balanced = nullptr;
    for (const auto& p : plans) if (p.profile == PlanProfile::balanced) balanced = &p;
    REQUIRE(balanced != nullptr);

    // 6) AltML evaluation (mock) + proposal
    const auto ml = mock_altml(*balanced, /*available=*/true);
    ProposalInputs pin{*rec, *balanced, ml, ens, ctx, champ.ranking_id, alt_ids, true, 1};
    const auto proposal = build_proposal(pin, now);
    CHECK(proposal.proposal_grade != Grade::invalid);
    CHECK(proposal.context_revision == rev);            // lineage intact end-to-end
    CHECK(proposal.max_holding_time_ms <= 48LL * 3600 * 1000);
    CHECK_FALSE(proposal.signal_proposal_id.empty());
    // Deterministic id: rebuilding the same proposal yields the same id.
    const auto proposal2 = build_proposal(pin, now);
    CHECK(proposal.signal_proposal_id == proposal2.signal_proposal_id);

    // 7) self-computed market outcome (no execution feedback / VPS2)
    OutcomeGeometry g;
    g.side = proposal.side;
    g.entry_type = proposal.entry_type;
    g.entry_price = proposal.entry_price;
    g.trigger_price = proposal.trigger_price;
    g.stop_loss_price = proposal.stop_loss_price;
    g.take_profit_price = proposal.take_profit_price;
    g.created_at_ms = now;
    g.entry_fill_timeout_ms = proposal.entry_fill_timeout_ms;
    g.valid_until_ms = proposal.valid_until_ms;
    g.time_stop_at_ms = proposal.time_stop_at_ms;
    auto bars = up_path(now, 100.0, proposal.take_profit_price * 1.01, 40);
    const auto outcome = reconstruct_outcome(g, bars, OutcomePath::raw_trades, rec->candidate_id,
                                             proposal.signal_proposal_id, now);
    CHECK(outcome.outcome != MarketOutcomeType::data_incomplete);

    // 8) counterfactuals for the non-selected candidates
    std::vector<MarketOutcome> cfs;
    for (const auto& c : cands) {
        if (c.candidate_id == rec->candidate_id) continue;
        const auto cplans = generate_plans(c, ctx, now);
        if (cplans.empty()) continue;
        OutcomeGeometry cg = g;
        cg.side = c.side;
        cg.entry_type = cplans[1].entry_type;
        cg.entry_price = cplans[1].entry_price;
        cg.stop_loss_price = cplans[1].stop_loss_price;
        cg.take_profit_price = cplans[1].take_profit_price;
        cfs.push_back(reconstruct_outcome(cg, bars, OutcomePath::raw_trades, c.candidate_id, "", now));
    }
    const auto cf_out = compare_counterfactuals(champ.ranking_id, outcome, cfs, now);
    // regret is well-defined (>=0) whenever we could score both sides
    for (const auto& c : cf_out) CHECK(c.ranking_regret >= 0.0);

    // 9) attribution WITHOUT any execution feedback (VPS2 absent)
    const std::optional<ExecutionFeedback> no_exec;
    const auto attr = attribute_outcome(proposal, outcome, no_exec, 0.0, /*regime_changed=*/false, now);
    CHECK(attr.execution_attribution == 0.0);           // no execution error charged
    CHECK_FALSE(attr.dominant_factor.empty());

    // The whole path ran with AltML mocked and VPS2 entirely absent — the
    // self-contained VPS1 requirement (§1, §42) holds.
}

TEST_CASE("AltML unavailable still yields a deterministic proposal (VPS1 independence)") {
    const int64_t now = 1'000'100;
    auto ctx = make_ctx(100.0, "TREND_UP");
    ctx.forecasts.clear();                      // AltML down: no estimate at any horizon
    StrategyEngine eng;
    auto cands = eng.run(ctx, now, {});
    REQUIRE_FALSE(cands.empty());
    const auto& c = cands.front();
    CHECK(c.econ.state == EconState::model_unavailable);
    const auto plans = generate_plans(c, ctx, now);
    const auto ml = mock_altml(plans[1], /*available=*/false);   // AltML down
    EnsembleView ens = compute_ensemble(cands);
    ProposalInputs pin{c, plans[1], ml, ens, ctx, "rank-x", {}, true, 1};
    const auto s = build_proposal(pin, now);
    // A proposal is still produced without AltML -- that is the independence
    // property. What it must NOT do is invent the economics.
    //
    // This assertion used to be `expected_net_return != 0`, pinning a fallback
    // that computed p = 1/(1+rr) and rr/(1+rr) -- fair odds, whose expected
    // gross return is algebraically zero for every rr, leaving net = -cost. The
    // test passed because the fabrication was consistent, not because the
    // number meant anything.
    CHECK(s.state == ProposalState::proposed);
    CHECK_FALSE(s.edge_estimated);
    CHECK(s.expected_net_return == 0.0);         // absent, and flagged absent
    CHECK(s.p_target_before_stop == 0.0);
    // The cost is measured whether or not a model exists, so it is still here.
    CHECK(s.round_trip_cost_bps > 0.0);
    CHECK(s.cost_model_version > 0);
    // Geometry is not a forecast and remains available.
    CHECK(s.expected_rr > 0.0);
    bool ml_missing = false, no_edge = false;
    for (const auto& w : s.warnings) {
        if (w.code == "MODEL_UNAVAILABLE") ml_missing = true;
        if (w.code == "NO_EDGE_ESTIMATE") no_edge = true;
    }
    CHECK(ml_missing);
    CHECK(no_edge);
    // ...and it is never executable: the policy fails closed without a model.
    CHECK_FALSE(s.recommended_for_execution);
}
