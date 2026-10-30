// The economic decision policy: every verdict, both sides, fail-closed.

#include "altstrategy/engine/economic_policy.hpp"
#include "altstrategy/engine/signal_predictions.hpp"
#include "altstrategy/engine/state_candidates.hpp"
#include "engine_test_helpers.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <cmath>
#include <optional>

using namespace altstrategy::engine;
using namespace altstrategy::engine::test;
using Catch::Approx;

// The model-only path is OFF by default (an economic generator, not a market
// event); these tests exercise its logic explicitly switched on.
static const StateCandidateConfig kStateOn{true};

namespace {
constexpr int64_t kNow = 1'000'050;
constexpr int64_t kMin = 60'000;
}

TEST_CASE("a setup requires its exact supported payoff horizon", "[policy][horizon]") {
    CHECK(evaluation_horizon_minutes(15 * kMin) == 15);
    CHECK(evaluation_horizon_minutes(60 * kMin) == 60);
    CHECK(evaluation_horizon_minutes(240 * kMin) == 240);
    for (const auto duration : {int64_t{0}, int64_t{-1}, 15 * kMin + 1, 20 * kMin,
                               90 * kMin, 180 * kMin, 480 * kMin, 48 * 60 * kMin}) {
        CHECK(evaluation_horizon_minutes(duration) == 0);
    }
    const auto decision = evaluate_economics(Side::long_, 480 * kMin, make_ctx(), kNow, {});
    CHECK(decision.state == EconState::model_unavailable);
    CHECK(decision.reason_code == "PAYOFF_HORIZON_UNSUPPORTED");
    CHECK_FALSE(decision.actionable());
}

TEST_CASE("LONG and SHORT read their own side of the estimate", "[policy]") {
    auto ctx = make_ctx();
    ctx.forecasts.clear();
    ctx.forecasts[60] = econ_forecast(60, 7.0, -39.0);
    const auto l = evaluate_economics(Side::long_, 60 * kMin, ctx, kNow, {});
    CHECK(l.state == EconState::positive_edge);
    CHECK(l.expected_net_bps == 7.0);
    CHECK(l.expected_gross_bps == 23.0);
    CHECK(l.p_side == 0.40);
    CHECK(l.p_opposite == 0.15);
    const auto s = evaluate_economics(Side::short_, 60 * kMin, ctx, kNow, {});
    CHECK(s.state == EconState::no_edge);
    CHECK(s.expected_net_bps == -39.0);
    CHECK(s.p_side == 0.15);

    // Mirror image: the short side pays.
    ctx.forecasts[60] = econ_forecast(60, -41.0, 9.0);
    CHECK(evaluate_economics(Side::short_, 60 * kMin, ctx, kNow, {}).state == EconState::positive_edge);
    CHECK(evaluate_economics(Side::long_, 60 * kMin, ctx, kNow, {}).state == EconState::no_edge);
}

TEST_CASE("a small expected move does not pay for a high cost; a large one does", "[policy]") {
    auto ctx = make_ctx();
    ctx.forecasts.clear();
    // Gross +14 bps against a 16 bps round trip: NEUTRAL in effect, no edge.
    ctx.forecasts[15] = econ_forecast(15, -2.0, -30.0);
    CHECK(evaluate_economics(Side::long_, 15 * kMin, ctx, kNow, {}).state == EconState::no_edge);
    // A large expected move clears it.
    ctx.forecasts[15] = econ_forecast(15, 25.0, -57.0);
    CHECK(evaluate_economics(Side::long_, 15 * kMin, ctx, kNow, {}).actionable());
}

TEST_CASE("every way of not having a usable model is its own refusal", "[policy][failclosed]") {
    auto ctx = make_ctx();
    SECTION("missing") {
        ctx.forecasts.erase(240);
        const auto d = evaluate_economics(Side::long_, 4 * 60 * kMin, ctx, kNow, {});
        CHECK(d.state == EconState::model_unavailable);
        CHECK_FALSE(d.evaluated());
    }
    SECTION("stale") {
        ctx.forecasts[240].bar_open_time_ms = kNow - 10 * kMin;   // closed 9 minutes ago
        const auto d = evaluate_economics(Side::long_, 4 * 60 * kMin, ctx, kNow, {});
        CHECK(d.state == EconState::model_stale);
    }
    SECTION("future bar cannot pass the age subtraction") {
        ctx.forecasts[240].bar_open_time_ms = kNow + kMin;
        CHECK(evaluate_economics(Side::long_, 4 * 60 * kMin, ctx, kNow, {}).state ==
              EconState::model_stale);
    }
    SECTION("future forecast computation is unavailable") {
        ctx.forecasts[240].computed_at_ms = kNow + 1;
        CHECK(evaluate_economics(Side::long_, 4 * 60 * kMin, ctx, kNow, {}).state ==
              EconState::model_stale);
    }
    SECTION("not an econ estimate") {
        ctx.forecasts[240].econ = false;
        CHECK(evaluate_economics(Side::long_, 4 * 60 * kMin, ctx, kNow, {}).state ==
              EconState::model_unavailable);
    }
    SECTION("probabilities that are not a distribution") {
        ctx.forecasts[240].p_neutral = 0.9;
        CHECK(evaluate_economics(Side::long_, 4 * 60 * kMin, ctx, kNow, {}).state ==
              EconState::model_unavailable);
    }
    SECTION("an estimate filed under the wrong horizon") {
        ctx.forecasts[240].horizon_minutes = 60;
        CHECK(evaluate_economics(Side::long_, 4 * 60 * kMin, ctx, kNow, {}).state ==
              EconState::model_unavailable);
    }
    SECTION("abstained") {
        ctx.forecasts[240].abstain = true;
        CHECK(evaluate_economics(Side::long_, 4 * 60 * kMin, ctx, kNow, {}).state ==
              EconState::model_abstain);
    }
    SECTION("no side") {
        CHECK(evaluate_economics(Side::unspecified, 4 * 60 * kMin, ctx, kNow, {}).state ==
              EconState::model_unavailable);
    }
}

TEST_CASE("models that disagree about direction across horizons are a conflict", "[policy]") {
    auto ctx = make_ctx();
    ctx.forecasts.clear();
    ctx.forecasts[60] = econ_forecast(60, 6.0, -38.0);     // 1h: long pays
    ctx.forecasts[240] = econ_forecast(240, -50.0, 12.0);  // 4h: short pays
    const auto d = evaluate_economics(Side::long_, 60 * kMin, ctx, kNow, {});
    CHECK(d.state == EconState::horizon_conflict);
    CHECK_FALSE(d.actionable());
    // Agreement across horizons is not a conflict.
    ctx.forecasts[240] = econ_forecast(240, 3.0, -35.0);
    CHECK(evaluate_economics(Side::long_, 60 * kMin, ctx, kNow, {}).state == EconState::positive_edge);
    // A STALE opposite estimate does not veto a fresh one.
    ctx.forecasts[240] = econ_forecast(240, -50.0, 12.0, kNow - 20 * kMin);
    CHECK(evaluate_economics(Side::long_, 60 * kMin, ctx, kNow, {}).state == EconState::positive_edge);
}

TEST_CASE("every candidate carries the verdict, whatever its lifecycle", "[policy]") {
    StrategyEngine eng;
    auto ctx = make_ctx(100.0, "TREND_UP");
    CatalogueState st;
    for (const auto& m : eng.metas()) st.lifecycle[m.strategy_id] = Lifecycle::research;
    const auto cands = eng.run(ctx, kNow, st);
    REQUIRE_FALSE(cands.empty());
    for (const auto& c : cands) {
        INFO(c.strategy_id);
        CHECK(c.econ.horizon_minutes == evaluation_horizon_minutes(c.expected_horizon_ms));
        CHECK_FALSE(c.econ.reason_code.empty());
        CHECK(c.econ.evaluated());   // the fixture has fresh estimates for every horizon
    }
}

TEST_CASE("a SHADOW model's estimate is recorded but never actionable", "[policy][failclosed]") {
    auto ctx = make_ctx();
    ctx.forecasts.clear();
    ctx.forecasts[60] = econ_forecast(60, 30.0, -62.0);   // a large positive long estimate...
    ctx.forecasts[60].quality_status = "SHADOW";           // ...from a gate-rejected model
    const auto d = evaluate_economics(Side::long_, 60 * kMin, ctx, kNow, {});
    CHECK(d.state == EconState::model_shadow);
    CHECK_FALSE(d.actionable());
    CHECK_FALSE(d.evaluated());
    CHECK(d.has_estimate());
    CHECK(d.expected_net_bps == 30.0);                     // kept for learning
    // A shadow estimate on another horizon cannot veto an ACTIVE one either.
    ctx.forecasts[240] = econ_forecast(240, 5.0, -37.0);
    ctx.forecasts[60] = econ_forecast(60, -60.0, 20.0);
    ctx.forecasts[60].quality_status = "SHADOW";
    CHECK(evaluate_economics(Side::long_, 4 * 60 * kMin, ctx, kNow, {}).state == EconState::positive_edge);
}

TEST_CASE("an estimate scored with aligned inputs missing is recorded but never actionable",
          "[policy][failclosed]") {
    // After an AltML restart the 15m / 1h blocks were absent from every
    // prediction for up to an hour; LightGBM still answered, off-distribution.
    for (const Side side : {Side::long_, Side::short_}) {
        auto ctx = make_ctx();
        ctx.forecasts.clear();
        const bool is_long = side == Side::long_;
        ctx.forecasts[60] = is_long ? econ_forecast(60, 30.0, -62.0) : econ_forecast(60, -62.0, 30.0);
        ctx.forecasts[60].aligned_missing = 1;
        const auto d = evaluate_economics(side, 60 * kMin, ctx, kNow, {});
        CHECK(d.state == EconState::no_data);
        CHECK(d.reason_code == "NO_DATA");
        CHECK_FALSE(d.actionable());
        CHECK_FALSE(d.evaluated());
        CHECK(d.has_estimate());
        CHECK(d.expected_net_bps == 30.0);                 // kept for learning
        // Complete inputs: the same numbers act.
        ctx.forecasts[60].aligned_missing = 0;
        CHECK(evaluate_economics(side, 60 * kMin, ctx, kNow, {}).actionable());
    }
    // An incomplete estimate on another horizon cannot veto a complete one.
    auto ctx = make_ctx();
    ctx.forecasts.clear();
    ctx.forecasts[240] = econ_forecast(240, 5.0, -37.0);
    ctx.forecasts[60] = econ_forecast(60, -60.0, 20.0);
    ctx.forecasts[60].aligned_missing = 2;
    CHECK(evaluate_economics(Side::long_, 4 * 60 * kMin, ctx, kNow, {}).state == EconState::positive_edge);
}

TEST_CASE("edge strength is measured against the validated margin and the cost",
          "[policy][edge-states]") {
    // cost 16 bps, margin 5 bps: NO <= 0 < SMALL <= 5 < POSITIVE < 21 <= STRONG
    const std::optional<double> m = 5.0;
    CHECK(classify_edge(-0.1, 16.0, m) == EconState::no_edge);
    CHECK(classify_edge(0.0, 16.0, m) == EconState::no_edge);
    CHECK(classify_edge(0.3, 16.0, m) == EconState::small_edge);     // "> 0 by rounding" is not an edge
    CHECK(classify_edge(5.0, 16.0, m) == EconState::small_edge);
    CHECK(classify_edge(5.1, 16.0, m) == EconState::positive_edge);
    CHECK(classify_edge(20.9, 16.0, m) == EconState::positive_edge);
    CHECK(classify_edge(21.0, 16.0, m) == EconState::strong_edge);
    // No validated margin: a positive estimate is never more than SMALL.
    CHECK(classify_edge(100.0, 16.0, std::nullopt) == EconState::small_edge);
    CHECK(classify_edge(100.0, 16.0, std::optional<double>(std::nan(""))) == EconState::small_edge);
}

TEST_CASE("a schema-2 estimate carries no margin and is never actionable, either side",
          "[policy][failclosed][edge-states]") {
    for (const Side side : {Side::long_, Side::short_}) {
        auto ctx = make_ctx();
        ctx.forecasts.clear();
        const bool is_long = side == Side::long_;
        ctx.forecasts[60] = is_long ? econ_forecast(60, 40.0, -72.0) : econ_forecast(60, -72.0, 40.0);
        ctx.forecasts[60].prediction_schema_version = 2;
        ctx.forecasts[60].policy_margin_bps.reset();
        const auto d = evaluate_economics(side, 60 * kMin, ctx, kNow, {});
        CHECK(d.state == EconState::small_edge);
        CHECK_FALSE(d.actionable());
        CHECK(d.evaluated());
        // the same estimate with a validated margin is STRONG (40 >= 0 + 16)
        ctx.forecasts[60].policy_margin_bps = 0.0;
        CHECK(evaluate_economics(side, 60 * kMin, ctx, kNow, {}).state == EconState::strong_edge);
    }
}

// ---------------------------------------------------------------------------
// Full-state decisions (state_candidates.hpp)
// ---------------------------------------------------------------------------
#include "altstrategy/engine/state_candidates.hpp"
#include "altstrategy/engine/trade_plan.hpp"

TEST_CASE("the model acts on a state no detector described, both sides",
          "[state][selection-bias]") {
    for (const Side side : {Side::long_, Side::short_}) {
        auto ctx = make_ctx();
        ctx.forecasts.clear();
        const bool is_long = side == Side::long_;
        auto f = is_long ? econ_forecast(60, 12.0, -44.0) : econ_forecast(60, -44.0, 12.0);
        f.favorable_long_bps = 40; f.adverse_long_bps = -25;
        f.favorable_short_bps = 25; f.adverse_short_bps = -40;
        ctx.forecasts[60] = f;
        const auto c = state_candidates(ctx, kNow, {}, kStateOn, /*setups=*/{}, nullptr);
        REQUIRE(c.size() == 1);
        CHECK(c[0].strategy_id == kStateStrategyId);
        CHECK(c[0].side == side);
        CHECK(c[0].econ.actionable());
        CHECK(c[0].selection_mode == SelectionMode::exploit);
        CHECK(c[0].expected_horizon_ms == 60 * kMin);
        const double px = ctx.last_price();
        if (is_long) {
            CHECK(c[0].invalidation_hint < px);
            CHECK(c[0].target_hint > px);
            CHECK(c[0].target_hint == Approx(px * 1.004));
        } else {
            CHECK(c[0].invalidation_hint > px);
            CHECK(c[0].target_hint < px);
        }
    }
}

TEST_CASE("no state candidate without a validated edge; shadow is observed, not executed",
          "[state][failclosed]") {
    auto ctx = make_ctx();
    ctx.forecasts.clear();
    ctx.forecasts[60] = econ_forecast(60, 12.0, -44.0);
    ctx.forecasts[60].policy_margin_bps.reset();          // no validated margin: SMALL_EDGE
    CHECK(state_candidates(ctx, kNow, {}, kStateOn, {}, nullptr).empty());
    ctx.forecasts[60].policy_margin_bps = 20.0;            // edge 12 <= margin 20: SMALL
    CHECK(state_candidates(ctx, kNow, {}, kStateOn, {}, nullptr).empty());
    ctx.forecasts[60].policy_margin_bps = 5.0;
    ctx.forecasts[60].quality_status = "SHADOW";           // a shadow model clearing its margin
    const auto c = state_candidates(ctx, kNow, {}, kStateOn, {}, nullptr);
    REQUIRE(c.size() == 1);
    CHECK_FALSE(c[0].econ.actionable());
    CHECK(c[0].selection_mode == SelectionMode::explore_shadow);
    CHECK(c[0].lifecycle_status == Lifecycle::shadow);
    StateCandidateConfig off;
    off.enabled = false;
    CHECK(state_candidates(ctx, kNow, {}, off, {}, nullptr).empty());
}

TEST_CASE("setups become tags of the state; the caller can hold one thesis per horizon",
          "[state]") {
    auto ctx = make_ctx();
    ctx.forecasts.clear();
    ctx.forecasts[60] = econ_forecast(60, 12.0, -44.0);
    StrategyCandidate setup;
    setup.strategy_id = "donchian_breakout_continuation";
    setup.side = Side::long_;
    StrategyCandidate other = setup;
    other.strategy_id = "vwap_reclaim_continuation";
    other.side = Side::short_;
    const auto c = state_candidates(ctx, kNow, {}, kStateOn, {setup, other}, nullptr);
    REQUIRE(c.size() == 1);
    const auto& rc = c[0].reason_codes;
    CHECK(std::find(rc.begin(), rc.end(), "setup:donchian_breakout_continuation") != rc.end());
    CHECK(std::find(rc.begin(), rc.end(), "setup:vwap_reclaim_continuation") == rc.end());
    int calls = 0;
    const auto refuse = [&](int32_t, Side) { ++calls; return false; };
    CHECK(state_candidates(ctx, kNow, {}, kStateOn, {}, refuse).empty());
    CHECK(calls == 1);   // consulted only for a candidate that would exist
}

TEST_CASE("a state thesis enters at market: the estimate is for a taker entry", "[state][plan]") {
    auto ctx = make_ctx();
    ctx.forecasts.clear();
    ctx.forecasts[60] = econ_forecast(60, 12.0, -44.0);
    const auto c = state_candidates(ctx, kNow, {}, kStateOn, {}, nullptr);
    REQUIRE(c.size() == 1);
    CHECK(choose_entry(c[0], ctx) == EntryType::market);
}

// ---------------------------------------------------------------------------
// The market view a VPS1 signal carries (signal_predictions.hpp).
// ---------------------------------------------------------------------------
TEST_CASE("every served horizon is reported, with its condition, never a stand-in number",
          "[predictions][vps1][regression]") {
    using namespace altstrategy::engine;
    using namespace altstrategy::engine::test;
    auto ctx = make_ctx(100.0);
    const int64_t now = 1'000'050;
    const int64_t max_age = EconomicPolicyConfig{}.max_forecast_age_ms;
    ctx.forecasts[15].quality_status = "SHADOW";
    ctx.forecasts[60].aligned_missing = 2;
    ctx.forecasts.erase(240);
    auto hs = market_predictions(ctx, now, max_age);
    REQUIRE(hs.size() == 3);
    CHECK(hs[0].state == PredictionState::present);
    CHECK(hs[0].role == "SHADOW");                      // labelled, not dropped
    CHECK(hs[0].p_up == Catch::Approx(0.40));
    CHECK(hs[1].state == PredictionState::incomplete);
    CHECK(hs[1].has_values());
    CHECK(hs[2].state == PredictionState::missing);
    CHECK_FALSE(hs[2].has_values());
    CHECK(hs[2].p_up == 0.0);

    // Stale: older than the freshness bound, numbers kept with their state.
    auto stale = market_predictions(ctx, 960'000 + 60'000 + max_age + 1, max_age);
    CHECK(stale[0].state == PredictionState::stale);
    CHECK(stale[0].has_values());

    // Abstain and incompatible carry no numbers.
    ctx.forecasts[15].abstain = true;
    ctx.forecasts[60].p_long = 0.9;                     // probabilities no longer sum to one
    hs = market_predictions(ctx, now, max_age);
    CHECK(hs[0].state == PredictionState::abstain);
    CHECK_FALSE(hs[0].has_values());
    CHECK(hs[1].state == PredictionState::incompatible);
    CHECK_FALSE(hs[1].has_values());
}

TEST_CASE("schema-3 excursions are read as magnitudes, schema-4 ones as published",
          "[predictions][vps1]") {
    using namespace altstrategy::engine;
    using namespace altstrategy::engine::test;
    auto ctx = make_ctx(100.0);
    auto& f = ctx.forecasts[60];
    // Neither magnitude was on the wire: the prediction says so instead of
    // reporting "no move expected".
    auto h = horizon_prediction(ctx, 60, 1'000'050, EconomicPolicyConfig{}.max_forecast_age_ms);
    CHECK(h.has_values());
    CHECK_FALSE(h.has_excursions);
    CHECK_FALSE(side_excursions(h, Side::long_).has_value());
    f.has_side_bps = true;
    f.favorable_long_bps = 42.0;
    f.adverse_long_bps = -37.0;
    h = horizon_prediction(ctx, 60, 1'000'050, EconomicPolicyConfig{}.max_forecast_age_ms);
    CHECK(h.has_excursions);
    CHECK(h.expected_up_excursion_bps == Catch::Approx(42.0));
    CHECK(h.expected_down_excursion_bps == Catch::Approx(37.0));
    CHECK_FALSE(h.probability_calibrated);
    f.has_excursions = true;
    f.expected_up_excursion_bps = 50.0;
    f.expected_down_excursion_bps = 45.0;
    f.probability_calibrated = true;
    h = horizon_prediction(ctx, 60, 1'000'050, EconomicPolicyConfig{}.max_forecast_age_ms);
    CHECK(h.expected_up_excursion_bps == Catch::Approx(50.0));
    CHECK(h.expected_down_excursion_bps == Catch::Approx(45.0));
    CHECK(h.probability_calibrated);
}

TEST_CASE("model-only state candidates are off unless switched on", "[state][vps1]") {
    CHECK_FALSE(StateCandidateConfig{}.enabled);
    auto ctx = make_ctx(100.0);
    CHECK(state_candidates(ctx, 1'000'050, {}, StateCandidateConfig{}, {}, nullptr).empty());
}
