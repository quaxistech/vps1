// Ensemble, adaptive allocator (Bayesian shrinkage + time decay) and ranking
// (champion/challenger, conflict) tests (§10, §41).

#include "altstrategy/engine/allocator.hpp"
#include "altstrategy/engine/ensemble.hpp"
#include "altstrategy/engine/cost_model.hpp"
#include "altstrategy/engine/ranking.hpp"
#include "altstrategy/engine/trade_plan.hpp"
#include "altstrategy/engine/strategy_engine.hpp"
#include "engine_test_helpers.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace altstrategy::engine;
using namespace altstrategy::engine::test;

// The economic verdict a candidate carries into ranking (economic_policy.hpp).
static void set_econ(StrategyCandidate& c, EconState state, double net_bps, int32_t horizon = 60) {
    c.econ.state = state;
    c.econ.horizon_minutes = horizon;
    c.econ.expected_net_bps = net_bps;
    c.econ.round_trip_cost_bps = 16.0;
    c.econ.expected_gross_bps = net_bps + 16.0;
}

static StrategyCandidate cand(const std::string& id, Family fam, Side side, double score) {
    StrategyCandidate c;
    c.candidate_id = "c-" + id;
    c.strategy_id = id;
    c.strategy_version = "1.0.0";
    c.strategy_family = fam;
    c.side = side;
    c.static_score = score;
    c.adaptive_score = score;
    c.entry_hint = 100; c.invalidation_hint = 99; c.target_hint = 102;
    c.holding_class = HoldingClass::intraday;
    return c;
}

TEST_CASE("ensemble discounts a same-family pile as low independence (§11)") {
    std::vector<StrategyCandidate> same = {cand("a", Family::trend, Side::long_, 0.6),
                                           cand("b", Family::trend, Side::long_, 0.6),
                                           cand("c", Family::trend, Side::long_, 0.6)};
    const auto e1 = compute_ensemble(same);
    CHECK(e1.ensemble_direction == Side::long_);
    CHECK(e1.family_independence_score == Catch::Approx(1.0 / 3.0));

    std::vector<StrategyCandidate> diverse = {cand("a", Family::trend, Side::long_, 0.6),
                                              cand("b", Family::breakout, Side::long_, 0.6),
                                              cand("c", Family::vwap, Side::long_, 0.6)};
    const auto e2 = compute_ensemble(diverse);
    CHECK(e2.family_independence_score == Catch::Approx(1.0));   // three distinct families
}

TEST_CASE("ensemble marks conflicting families and agreement") {
    std::vector<StrategyCandidate> mixed = {cand("a", Family::trend, Side::long_, 0.8),
                                            cand("b", Family::mean_reversion, Side::short_, 0.4)};
    const auto e = compute_ensemble(mixed);
    CHECK(e.ensemble_direction == Side::long_);
    CHECK(e.ensemble_agreement > 0.5);
    CHECK(e.conflicting_strategies.size() == 1);
}

TEST_CASE("Bayesian shrinkage: a thin cell shrinks to its parent, a rich cell trusts itself") {
    PosteriorEstimator est;
    StatCell parent; parent.n = 400; parent.mean_edge = 0.001; parent.var = 0.01;
    StatCell thin; thin.n = 2; thin.mean_edge = 0.05; thin.var = 0.01;    // two lucky wins
    StatCell rich; rich.n = 300; rich.mean_edge = 0.05; rich.var = 0.01;

    const auto p_thin = est.estimate("s", "1", thin, {parent}, 0.0);
    const auto p_rich = est.estimate("s", "1", rich, {parent}, 0.0);
    // The thin cell is pulled far toward the parent (~0.001); the rich cell stays high.
    CHECK(p_thin.posterior_mean_edge < 0.02);
    CHECK(p_rich.posterior_mean_edge > 0.03);
    CHECK(p_thin.shrinkage_factor < p_rich.shrinkage_factor);
    // A strong positive edge with plenty of samples earns weight > 1.
    CHECK(p_rich.adaptive_weight > 1.0);
}

TEST_CASE("time decay downweights stale observations") {
    PosteriorEstimator est;
    const int64_t now = 100LL * 86400 * 1000;
    // One fresh, one 120-day-old observation.
    const auto c = est.decay({{now, 0.02}, {now - 120LL * 86400 * 1000, -0.02}}, now);
    // Effective N is well under 2 and the mean leans toward the fresh (+) value.
    CHECK(c.n < 2.0);
    CHECK(c.mean_edge > 0.0);
}

TEST_CASE("allocator refines adaptive_score from the posterior table with fallback") {
    AdaptiveAllocator alloc;
    PosteriorTable table;
    StrategyPosterior p;
    p.strategy_id = "momentum_continuation"; p.strategy_version = "1.0.0";
    p.regime = "*"; p.symbol_group = "*"; p.side = Side::unspecified;
    p.holding_class = HoldingClass::unspecified;
    p.adaptive_weight = 1.8; p.posterior_mean_edge = 0.01; p.effective_sample_size = 200;
    table.put(p);

    auto c = cand("momentum_continuation", Family::trend, Side::long_, 0.5);
    c.global_regime = "TREND_UP";
    alloc.allocate(c, "majors", table);
    CHECK(c.adaptive_score == Catch::Approx(0.5 * 1.8));   // fell back to strategy-global posterior
    CHECK(c.effective_sample_size == Catch::Approx(200));
}

TEST_CASE("ranking picks a clear winner and champion/challenger differ") {
    auto ctx = make_ctx();
    // The champion ranks on the ML expected net; the setup scores are the
    // other way round on purpose: a high static score no longer wins.
    std::vector<StrategyCandidate> cands = {cand("strong", Family::trend, Side::long_, 0.2),
                                            cand("weak", Family::vwap, Side::long_, 0.9)};
    set_econ(cands[0], EconState::positive_edge, 12.0);
    set_econ(cands[1], EconState::no_edge, -5.0);
    for (auto& c : cands) { c.ml_context = ctx.market_forecast; c.global_regime = "TREND_UP"; }
    const auto ens = compute_ensemble(cands);

    const auto champ = rank(cands, ctx, ens, default_champion_policy(), 1000);
    REQUIRE(champ.ranked_candidates.size() == 2);
    CHECK(champ.recommended_candidate_id == "c-strong");
    CHECK(champ.result == RankingResult::recommended);
    CHECK_FALSE(champ.is_challenger);
    CHECK(champ.ranking_policy_id == "champion_ml_v2");

    const auto chall = rank(cands, ctx, ens, default_challenger_policy(), 1000);
    CHECK(chall.is_challenger);
    CHECK(chall.ranking_id != champ.ranking_id);   // distinct shadow ranking
}

TEST_CASE("ranking prices the canonical round-trip cost, not a second formula") {
    // ranking_components() computed `2 * fees_bps + spread_bps +
    // expected_slippage_bps`. Two errors in one line: fees_bps is a fixed 4.0
    // that cannot tell a maker entry from a taker one (the real round trip is 8
    // or 12 bps), and expected_slippage_bps already carries half the spread, so
    // the spread was charged 1.5 times -- the exact double-count cost_model.hpp
    // records having removed on its own side.
    auto ctx = make_ctx();
    ctx.spread_bps = 6.0;
    ctx.expected_slippage_bps = 0.5 * ctx.spread_bps + 2.0;   // half spread + depth
    ctx.top_depth_usdt = 10'000;                              // too thin for a market entry

    // A pullback-style candidate rests a limit order: maker entry, taker exit.
    auto limit_cand = cand("trend_pullback", Family::trend, Side::long_, 0.5);
    limit_cand.global_regime = "TREND_UP";
    const auto ens = compute_ensemble({limit_cand});
    const auto comps = ranking_components(limit_cand, ctx, ens);

    // Canonical: fees 2 + 6 = 8, spread 3 (exit only), depth impact 2 => 13 bps.
    const TradeCost expected = compute_trade_cost(choose_entry(limit_cand, ctx), ctx);
    CHECK(expected.round_trip_bps == Catch::Approx(13.0));
    CHECK(comps.at("cost_penalty") == Catch::Approx(13.0 / 30.0));
    // The retired formula would have said 2*4 + 6 + 5 = 19 bps.
    CHECK(comps.at("cost_penalty") != Catch::Approx(19.0 / 30.0));
}

TEST_CASE("ranking is deterministic when scores tie") {
    // Comparing final_score alone left equal scores to std::sort, which is not
    // stable: the same context could order the same two candidates either way
    // and recommend a different one, with nothing in the record to explain it.
    // No tie has occurred in seven days of production -- which is why it has to
    // be pinned before two strategies start agreeing rather than after.
    auto ctx = make_ctx();
    std::vector<StrategyCandidate> cands = {cand("zeta", Family::trend, Side::long_, 0.6),
                                            cand("alpha", Family::trend, Side::long_, 0.6)};
    for (auto& c : cands) { c.ml_context = ctx.market_forecast; c.global_regime = "TREND_UP"; }
    const auto ens = compute_ensemble(cands);

    const auto first = rank(cands, ctx, ens, default_champion_policy(), 1000);
    REQUIRE(first.ranked_candidates.size() == 2);
    REQUIRE(first.ranked_candidates[0].final_score == first.ranked_candidates[1].final_score);

    // Same input in the opposite order must produce the same ordering.
    std::vector<StrategyCandidate> reversed = {cands[1], cands[0]};
    const auto second = rank(reversed, ctx, ens, default_champion_policy(), 1000);
    REQUIRE(second.ranked_candidates.size() == 2);
    CHECK(first.ranked_candidates[0].candidate_id == second.ranked_candidates[0].candidate_id);
    CHECK(first.ranked_candidates[1].candidate_id == second.ranked_candidates[1].candidate_id);
    CHECK(first.recommended_candidate_id == second.recommended_candidate_id);
    // Tie broken on the stable candidate_id, not on insertion order.
    CHECK(first.ranked_candidates[0].candidate_id == "c-alpha");
}

TEST_CASE("ranking flags a conflict when top two are close and opposite") {
    auto ctx = make_ctx();
    // Two setups on different horizons, each with a positive verdict for
    // OPPOSITE sides: the champion trusts neither and recommends nothing.
    std::vector<StrategyCandidate> cands = {cand("a", Family::trend, Side::long_, 0.5),
                                            cand("b", Family::mean_reversion, Side::short_, 0.5)};
    set_econ(cands[0], EconState::positive_edge, 6.0, 240);
    set_econ(cands[1], EconState::positive_edge, 5.0, 15);
    for (auto& c : cands) c.global_regime = "RANGE";
    const auto ens = compute_ensemble(cands);
    const auto r = rank(cands, ctx, ens, default_champion_policy(), 1000, 2.0);
    CHECK(r.result == RankingResult::conflicted);
    CHECK(r.recommended_candidate_id.empty());
}

TEST_CASE("empty candidate set ranks as NO_CANDIDATES") {
    auto ctx = make_ctx();
    const auto r = rank({}, ctx, {}, default_champion_policy(), 1000);
    CHECK(r.result == RankingResult::no_candidates);
}

// Regression: SHADOW was a one-way door. `rank_candidates` correctly refuses to
// make an explore_shadow candidate the executable recommendation, but the
// decision path read an empty recommendation as "produce nothing", so a shadow
// strategy generated candidates and never a proposal — hence never an outcome,
// never a posterior, and never the `sample_count` its own promotion rule
// requires. Both halves of that contract are asserted here.
TEST_CASE("shadow candidates are rankable but never executable",
          "[ranking][regression]") {
    auto ctx = make_ctx();

    const auto shadow_candidate = [&](const char* id, double score) {
        auto c = cand(id, Family::trend, Side::long_, score);
        c.global_regime = "TREND_UP";
        c.selection_mode = SelectionMode::explore_shadow;
        return c;
    };

    SECTION("an all-shadow field still produces a ranking to observe") {
        const std::vector<StrategyCandidate> cands = {shadow_candidate("a", 0.8),
                                                      shadow_candidate("b", 0.5)};
        const auto ens = compute_ensemble(cands);
        const auto rp = rank(cands, ctx, ens, default_champion_policy(), 1000);

        // Rankable: there is a top candidate to plan an observation from.
        REQUIRE(rp.ranked_candidates.size() == 2);
        REQUIRE_FALSE(rp.ranked_candidates.front().candidate_id.empty());

        // ...and not executable: no shadow candidate is ever recommended.
        REQUIRE(rp.recommended_candidate_id.empty());
    }

    SECTION("one exploit candidate among shadows is the recommendation") {
        std::vector<StrategyCandidate> cands = {shadow_candidate("a", 0.9),
                                                shadow_candidate("b", 0.5)};
        auto exploit = shadow_candidate("c", 0.7);
        exploit.selection_mode = SelectionMode::exploit;
        set_econ(exploit, EconState::positive_edge, 3.0);
        cands.push_back(exploit);

        const auto ens = compute_ensemble(cands);
        const auto rp = rank(cands, ctx, ens, default_champion_policy(), 1000);
        REQUIRE(rp.recommended_candidate_id == "c-c");
    }
}

TEST_CASE("an unobserved venue relation scores nothing, not a neutral half point") {
    // `venue_volatility_agreement` used to read
    // cross_venue.get("volatility_agreement", 0.5), so a context with NO
    // cross-venue vector at all still scored half a point of "the venues agree
    // about volatility". policy_score() skips components that are absent from
    // the map, so the fix is to emit the key only when the value was observed.
    auto ctx = make_ctx(100.0, "TREND_UP");
    ctx.cross_venue_features.values.clear();
    ctx.cross_venue_features.valid = false;

    StrategyEngine eng;
    const auto cands = eng.run(ctx, 1'000'100, {});
    REQUIRE_FALSE(cands.empty());
    const auto ens = compute_ensemble(cands);
    const auto k = ranking_components(cands.front(), ctx, ens);

    CHECK(k.find("venue_volatility_agreement") == k.end());
    CHECK(k.find("venue_trend_agreement") == k.end());
    CHECK(k.find("venue_rsi_disagreement") == k.end());

    // With the vector present, the components come back.
    ctx.cross_venue_features.values["volatility_agreement"] = 0.9;
    ctx.cross_venue_features.values["trend_alignment_agreement"] = 0.8;
    ctx.cross_venue_features.valid = true;
    const auto k2 = ranking_components(cands.front(), ctx, ens);
    CHECK(k2.count("venue_volatility_agreement") == 1);
    CHECK(k2.at("venue_volatility_agreement") == Catch::Approx(0.9));
}

TEST_CASE("a more convinced model raises a candidate; conviction is never a penalty (HISTORY-01)",
          "[ranking][regression]") {
    // AltML's confidence (|p_up - 0.5| * 2) spent its life as `ml_uncertainty`
    // under a negative weight, so the model's most confident calls ranked lowest.
    auto ctx = make_ctx(100.0, "TREND_UP");
    auto c = cand("a", Family::trend, Side::long_, 0.6);
    c.ml_context = ctx.market_forecast;
    REQUIRE(c.ml_context.present);
    REQUIRE_FALSE(c.ml_context.abstain);
    const auto ens = compute_ensemble({c});
    const auto score_at = [&](double conviction) {
        c.ml_context.conviction = conviction;
        const auto k = ranking_components(c, ctx, ens);
        CHECK(k.at("ml_conviction") == Catch::Approx(conviction));
        return policy_score(default_challenger_policy(), k);
    };
    CHECK(score_at(0.9) > score_at(0.1));

    const auto& w = default_challenger_policy().weights;
    CHECK(w.at("ml_conviction") > 0);
    CHECK(w.count("ml_uncertainty") == 0);
    for (const auto* penalty : {"posterior_uncertainty", "regime_divergence", "cost_penalty", "warning_penalty",
                                "venue_rsi_disagreement"})
        CHECK(w.at(penalty) < 0);

    // An abstaining model contributes no conviction, not a middling one.
    c.ml_context.conviction = 0.9;
    c.ml_context.abstain = true;
    CHECK(ranking_components(c, ctx, ens).at("ml_conviction") == 0.0);
}

TEST_CASE("the champion fails closed: no positive verdict, no recommendation", "[ranking][policy]") {
    auto ctx = make_ctx();
    SECTION("no model at all") {
        std::vector<StrategyCandidate> cands = {cand("a", Family::trend, Side::long_, 0.9)};
        set_econ(cands[0], EconState::model_unavailable, 0.0);
        const auto r = rank(cands, ctx, compute_ensemble(cands), default_champion_policy(), 1000);
        CHECK(r.recommended_candidate_id.empty());
        CHECK(r.result == RankingResult::no_clear_winner);
        // The ML input is ABSENT from the components, not a 0.0 score.
        CHECK(r.ranked_candidates.front().components.count("ml_expected_net_bps") == 0);
    }
    SECTION("stale, unavailable, abstained, no edge, small edge, no data, shadow") {
        for (const auto st : {EconState::model_stale, EconState::model_unavailable, EconState::model_abstain,
                              EconState::no_edge, EconState::small_edge, EconState::horizon_conflict,
                              EconState::no_data, EconState::model_shadow}) {
            std::vector<StrategyCandidate> cands = {cand("a", Family::trend, Side::long_, 0.9)};
            set_econ(cands[0], st, st == EconState::no_edge ? -2.0 : 0.0);
            const auto r = rank(cands, ctx, compute_ensemble(cands), default_champion_policy(), 1000);
            CHECK(r.recommended_candidate_id.empty());
        }
    }
    SECTION("an unevaluated candidate never outranks an evaluated one") {
        std::vector<StrategyCandidate> cands = {cand("nomodel", Family::trend, Side::long_, 0.99),
                                                cand("neg", Family::vwap, Side::long_, 0.10),
                                                cand("pos", Family::breakout, Side::short_, 0.10)};
        set_econ(cands[0], EconState::model_unavailable, 0.0);
        set_econ(cands[1], EconState::no_edge, -3.0);
        set_econ(cands[2], EconState::positive_edge, 4.0);
        const auto r = rank(cands, ctx, compute_ensemble(cands), default_champion_policy(), 1000);
        REQUIRE(r.ranked_candidates.size() == 3);
        CHECK(r.ranked_candidates[0].candidate_id == "c-pos");
        CHECK(r.ranked_candidates[1].candidate_id == "c-neg");
        CHECK(r.ranked_candidates[2].candidate_id == "c-nomodel");
        CHECK(r.recommended_candidate_id == "c-pos");
    }
    SECTION("the challenger still ranks by the old weights, and decides nothing") {
        std::vector<StrategyCandidate> cands = {cand("a", Family::trend, Side::long_, 0.9)};
        set_econ(cands[0], EconState::model_unavailable, 0.0);
        const auto r = rank(cands, ctx, compute_ensemble(cands), default_challenger_policy(), 1000);
        CHECK(r.is_challenger);
        CHECK(r.ranking_policy_id == "challenger_static_v1");
    }
}
