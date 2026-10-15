// Forecast -> plan geometry in its physical units and from its own side
// (QUALITY-01/02/03), on the venue's grid (DESIGN-02).

#include "altstrategy/engine/economic_policy.hpp"
#include "altstrategy/engine/proposal.hpp"
#include "altstrategy/engine/ranking.hpp"
#include "altstrategy/engine/signal_predictions.hpp"
#include "altstrategy/engine/trade_plan.hpp"
#include "altstrategy/engine/units.hpp"
#include "engine_test_helpers.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace altstrategy::engine;
using namespace altstrategy::engine::test;

namespace {

constexpr int64_t kNow = 1'000'050;

// A 4-hour trend setup on make_ctx (px 100, ATR 1.0, no projected zones) whose
// 4h prediction expects `up_bps` above and `down_bps` below the reference.
StrategyCandidate candidate(DecisionContext& ctx, Side side, double up_bps, double down_bps) {
    auto& f = ctx.forecasts[240];
    f.has_excursions = true;
    f.expected_up_excursion_bps = up_bps;
    f.expected_down_excursion_bps = down_bps;
    StrategyCandidate c;
    c.candidate_id = "cand-units";
    c.strategy_id = "momentum_continuation";
    c.strategy_version = "1.0.0";
    c.strategy_family = Family::trend;
    c.side = side;
    c.canonical_pair_id = ctx.canonical_pair_id;
    c.context_id = ctx.context_id;
    c.holding_class = HoldingClass::intraday;
    c.expected_horizon_ms = 4LL * 3600 * 1000;
    const double px = ctx.last_price();
    const double dir = side == Side::long_ ? 1.0 : -1.0;
    c.entry_hint = px;
    c.invalidation_hint = px - dir * 1.0;
    c.target_hint = px + dir * 2.0;
    c.raw_setup_score = 0.5;
    c.reason_codes = {"test_setup"};
    c.predictions = market_predictions(ctx, kNow, EconomicPolicyConfig{}.max_forecast_age_ms);
    c.econ = evaluate_economics(c.side, c.expected_horizon_ms, ctx, kNow, {});
    c.ml_context = ctx.forecasts[240];
    return c;
}

const TradePlanVariant& profile(const std::vector<TradePlanVariant>& plans, PlanProfile p) {
    for (const auto& v : plans)
        if (v.profile == p) return v;
    FAIL("profile missing");
    return plans.front();
}

bool has_warning(const TradeSignalProposal& s, const std::string& code) {
    for (const auto& w : s.warnings)
        if (w.code == code) return true;
    return false;
}

TradeSignalProposal propose(const StrategyCandidate& c, const TradePlanVariant& v, const DecisionContext& ctx) {
    const MlEvaluation ml;
    const EnsembleView ens;
    return build_proposal(ProposalInputs{c, v, ml, ens, ctx, "rank-1", {}, true, 1}, kNow);
}

} // namespace

TEST_CASE("bps, fractions and prices convert in one place, with the unit in the name", "[units]") {
    CHECK(units::bps_to_price_distance(100.0, 100.0) == Catch::Approx(1.0));          // 100 bps of 100 = 1 %
    CHECK(units::bps_to_price_distance(0.00001, 100.0) == Catch::Approx(1e-7));
    CHECK(units::bps_to_price_distance(100'000.0, 100.0) == Catch::Approx(1'000.0));
    CHECK(units::fraction_to_bps(0.01) == Catch::Approx(100.0));
    CHECK(units::bps_to_fraction(100.0) == Catch::Approx(0.01));
    CHECK(units::price_distance_to_bps(100.0, -1.0) == Catch::Approx(100.0));
    CHECK(std::isnan(units::price_distance_to_bps(0.0, 1.0)));
    CHECK(std::isnan(units::price_distance_to_bps(-5.0, 1.0)));
}

TEST_CASE("a forecast 100 bps excursion is a 1 % target, not a 0.0001 % one",
          "[plan][units][regression]") {
    // The defect: AltML's fraction was read as bps and divided by 10 000
    // again, so the target rounded onto the entry and was nudged one tick
    // away -- 0.1-1 bps against 15-20 bps of cost.
    auto ctx = make_ctx(100.0);
    const auto c = candidate(ctx, Side::long_, 100.0, 40.0);
    const auto plans = generate_plans(c, ctx, 2'000'000);
    REQUIRE(plans.size() == 3);
    const auto& bal = profile(plans, PlanProfile::balanced);
    CHECK(bal.target_basis == "ml_mfe");
    CHECK(bal.target_distance_bps == Catch::Approx(115.0).margin(0.5));   // 1.15 x 100 bps
    CHECK(profile(plans, PlanProfile::aggressive).target_distance_bps == Catch::Approx(150.0).margin(0.5));
    CHECK(profile(plans, PlanProfile::conservative).target_distance_bps == Catch::Approx(85.0).margin(0.5));
    for (const auto& v : plans) {
        CHECK(v.geometry_version == kPlanGeometryVersion);
        CHECK(v.target_distance_bps > propose(c, v, ctx).round_trip_cost_bps);
    }
}

TEST_CASE("LONG and SHORT each read their own side's excursions", "[plan][units][side][regression]") {
    // up 30 / down 150: favourable for a SHORT, adverse for a LONG.
    auto ctx_l = make_ctx(100.0);
    const auto lng = generate_plans(candidate(ctx_l, Side::long_, 30.0, 150.0), ctx_l, 2'000'000);
    auto ctx_s = make_ctx(100.0, "TREND_DOWN");
    const auto sht = generate_plans(candidate(ctx_s, Side::short_, 30.0, 150.0), ctx_s, 2'000'000);
    const auto& bl = profile(lng, PlanProfile::balanced);
    const auto& bs = profile(sht, PlanProfile::balanced);
    // LONG: target from the 30 bps up move, stop floored by the 150 bps down move.
    CHECK(bl.target_basis == "ml_mfe");
    // 1.15 x 30 bps, to the nearest whole tick of the 0.01 grid (1 bp here).
    CHECK(bl.target_distance_bps == Catch::Approx(34.5).margin(1.0));
    CHECK(bl.stop_basis == "ml_mae");
    CHECK(bl.stop_distance_bps == Catch::Approx(150.0).margin(0.5));
    // SHORT: the mirror -- the 150 bps DOWN move is its target.
    CHECK(bs.target_basis == "ml_mfe");
    CHECK(bs.target_distance_bps == Catch::Approx(172.5).margin(1.0));
    CHECK(bs.stop_distance_bps == Catch::Approx(100.0).margin(0.5));   // invalidation (1.0) beats 30 bps
    CHECK(bs.take_profit_price < bs.entry_price);
    CHECK(bs.stop_loss_price > bs.entry_price);

    // Mirrored markets give mirrored geometry: SHORT(up a, down b) == LONG(up b, down a).
    auto ctx_m = make_ctx(100.0);
    const auto mirror = generate_plans(candidate(ctx_m, Side::long_, 150.0, 30.0), ctx_m, 2'000'000);
    for (const auto p : {PlanProfile::aggressive, PlanProfile::balanced, PlanProfile::conservative}) {
        CHECK(profile(sht, p).target_distance_bps == Catch::Approx(profile(mirror, p).target_distance_bps));
        CHECK(profile(sht, p).stop_distance_bps == Catch::Approx(profile(mirror, p).stop_distance_bps));
    }
}

TEST_CASE("a forecast that may not shape a plan shapes nothing", "[plan][units][regression]") {
    auto base_ctx = make_ctx(100.0);
    auto no_ml = candidate(base_ctx, Side::long_, 100.0, 300.0);
    no_ml.predictions.clear();
    const auto reference = generate_plans(no_ml, base_ctx, 2'000'000);
    REQUIRE(profile(reference, PlanProfile::balanced).target_basis == "strategy_target");

    const auto check_inert = [&](auto&& spoil) {
        auto ctx = make_ctx(100.0);
        auto c = candidate(ctx, Side::long_, 100.0, 300.0);
        spoil(ctx, c);
        c.predictions = market_predictions(ctx, kNow, EconomicPolicyConfig{}.max_forecast_age_ms);
        const auto plans = generate_plans(c, ctx, 2'000'000);
        for (const auto p : {PlanProfile::aggressive, PlanProfile::balanced, PlanProfile::conservative}) {
            CHECK(profile(plans, p).target_basis != "ml_mfe");
            CHECK(profile(plans, p).stop_basis != "ml_mae");
            CHECK(profile(plans, p).take_profit_price == Catch::Approx(profile(reference, p).take_profit_price));
            CHECK(profile(plans, p).stop_loss_price == Catch::Approx(profile(reference, p).stop_loss_price));
        }
    };
    SECTION("only a SHADOW model answered") {
        check_inert([](DecisionContext& ctx, StrategyCandidate&) { ctx.forecasts[240].quality_status = "SHADOW"; });
    }
    SECTION("the prediction is stale") {
        check_inert([](DecisionContext& ctx, StrategyCandidate&) { ctx.forecasts[240].bar_open_time_ms = 600'000; });
    }
    SECTION("aligned inputs were missing") {
        check_inert([](DecisionContext& ctx, StrategyCandidate&) { ctx.forecasts[240].aligned_missing = 2; });
    }
    SECTION("the model abstained") {
        check_inert([](DecisionContext& ctx, StrategyCandidate&) { ctx.forecasts[240].abstain = true; });
    }
    SECTION("the forecast carried no magnitudes") {
        check_inert([](DecisionContext& ctx, StrategyCandidate&) { ctx.forecasts[240].has_excursions = false; });
    }
    SECTION("no prediction for the candidate's horizon") {
        check_inert([](DecisionContext& ctx, StrategyCandidate&) { ctx.forecasts.erase(240); });
    }
}

TEST_CASE("the venue's tick at T0 puts every price on its grid; a stop never rounds toward its entry",
          "[plan][tick][regression]") {
    auto ctx = make_ctx(100.0);
    ctx.bitget_features.values["atr_14"] = 0.6;   // floors: 0.48 ATR buffer, 0.6 invalidation
    ctx.price_tick = 0.5;                         // a coarse grid: one tick is 50 bps
    ctx.instrument_spec_version = 3;
    for (const auto side : {Side::long_, Side::short_}) {
        auto c = candidate(ctx, side, 0.0, 0.0);
        c.predictions.clear();
        const double dir = side == Side::long_ ? 1.0 : -1.0;
        c.invalidation_hint = c.entry_hint - dir * 0.6;
        const auto plans = generate_plans(c, ctx, 2'000'000);
        for (const auto& v : plans) {
            CHECK(v.tick_source == "instrument_spec");
            CHECK(v.tick_size == 0.5);
            for (const double price : {v.entry_price, v.stop_loss_price, v.take_profit_price}) {
                const double n = price / 0.5;
                CHECK(std::abs(n - std::round(n)) < 1e-9);
            }
        }
        // BALANCED stop = 0.6 below a 100.0 entry = 99.4: the nearest tick
        // (99.5) would sit INSIDE the 0.6 floor; the grid takes it to 99.0.
        const auto& bal = profile(plans, PlanProfile::balanced);
        CHECK(std::abs(bal.entry_price - bal.stop_loss_price) >= 0.6 - 1e-9);
        CHECK(bal.stop_loss_price == Catch::Approx(100.0 - dir * 1.0));
        CHECK_FALSE(has_warning(propose(c, bal, ctx), "TICK_SIZE_DERIVED"));
    }
    // No spec resolved: the plan falls back to a derived tick and says so.
    ctx.price_tick = 0;
    auto c = candidate(ctx, Side::long_, 0.0, 0.0);
    const auto derived = generate_plans(c, ctx, 2'000'000);
    const auto& bal = profile(derived, PlanProfile::balanced);
    CHECK(bal.tick_source == "derived");
    CHECK(has_warning(propose(c, bal, ctx), "TICK_SIZE_DERIVED"));
}

TEST_CASE("the price grid is the spec version in force at T0", "[plan][tick]") {
    const PriceTickTable table = {
        {7, {{1'000, 1, 0.0001}, {5'000, 2, 0.00001}, {5'000, 3, 0.001}, {9'000, 4, 0.0}}},
    };
    CHECK(price_tick_at(table, 7, 999) == nullptr);           // nothing in force yet
    CHECK(price_tick_at(table, 7, 1'000)->version == 1);
    CHECK(price_tick_at(table, 7, 4'999)->version == 1);
    CHECK(price_tick_at(table, 7, 5'000)->version == 3);      // same valid_from: the higher version
    CHECK(price_tick_at(table, 7, 99'999)->version == 3);     // a non-positive tick is no grid
    CHECK(price_tick_at(table, 8, 99'999) == nullptr);        // unknown pair
}

TEST_CASE("a plan whose target cannot pay its estimated cost says so, and is still published",
          "[proposal][economics][regression]") {
    auto ctx = make_ctx(100.0);
    const auto tiny = candidate(ctx, Side::long_, 5.0, 40.0);
    const auto tiny_plans = generate_plans(tiny, ctx, 2'000'000);
    const auto& small = profile(tiny_plans, PlanProfile::balanced);
    const auto s = propose(tiny, small, ctx);
    REQUIRE(small.target_distance_bps < s.round_trip_cost_bps);
    CHECK(has_warning(s, "PLAN_TARGET_BELOW_COST"));
    CHECK(s.proposal_grade != Grade::invalid);   // a market event is not refused for its economics
    CHECK(s.plan_geometry_version == kPlanGeometryVersion);

    auto ctx2 = make_ctx(100.0);
    const auto big = candidate(ctx2, Side::long_, 100.0, 40.0);
    const auto big_plans = generate_plans(big, ctx2, 2'000'000);
    const auto& wide = profile(big_plans, PlanProfile::balanced);
    CHECK_FALSE(has_warning(propose(big, wide, ctx2), "PLAN_TARGET_BELOW_COST"));
}

TEST_CASE("the basis names the source that actually set the level", "[plan][provenance]") {
    auto ctx = make_ctx(100.0);
    auto c = candidate(ctx, Side::long_, 0.0, 0.0);
    c.predictions.clear();
    CHECK(profile(generate_plans(c, ctx, 2'000'000), PlanProfile::balanced).target_basis == "strategy_target");
    c.target_hint = c.entry_hint;   // no strategy target: a multiple of the stop
    CHECK(profile(generate_plans(c, ctx, 2'000'000), PlanProfile::balanced).target_basis == "rr_target");
    // A wide spread makes the min-stop floor the binding one.
    ctx.spread_bps = 40.0;
    ctx.expected_slippage_bps = 20.0;
    c.invalidation_hint = c.entry_hint - 0.01;
    ctx.bitget_features.values["atr_14"] = 0.1;
    CHECK(profile(generate_plans(c, ctx, 2'000'000), PlanProfile::balanced).stop_basis == "min_stop_floor");
}

TEST_CASE("a market entry needs a large expected move for THIS side, from an ACTIVE model",
          "[plan][side][regression]") {
    // AltML's expected return is the LONG perspective: -40 bps favours a short.
    auto ctx = make_ctx(100.0);
    ctx.forecasts[240].expected_return = -0.0040;
    auto c = candidate(ctx, Side::long_, 60.0, 60.0);
    c.raw_setup_score = 0.8;
    c.predictions = market_predictions(ctx, kNow, EconomicPolicyConfig{}.max_forecast_age_ms);
    CHECK(choose_entry(c, ctx) == EntryType::limit);   // it used to be MARKET: |-0.004| > 0.003
    c.side = Side::short_;
    c.econ = evaluate_economics(c.side, c.expected_horizon_ms, ctx, kNow, {});
    CHECK(choose_entry(c, ctx) == EntryType::market);
    ctx.forecasts[240].quality_status = "SHADOW";
    c.predictions = market_predictions(ctx, kNow, EconomicPolicyConfig{}.max_forecast_age_ms);
    CHECK(choose_entry(c, ctx) == EntryType::limit);
}

TEST_CASE("the ranking's excursion ratio is the candidate's own side", "[ranking][side][regression]") {
    auto ctx = make_ctx(100.0);
    const auto lng = candidate(ctx, Side::long_, 40.0, 100.0);
    const auto sht = candidate(ctx, Side::short_, 40.0, 100.0);
    const EnsembleView ens;
    CHECK(ranking_components(lng, ctx, ens).at("ml_excursion_rr") ==
          Catch::Approx(std::tanh(40.0 / 100.0 - 1.0)));
    CHECK(ranking_components(sht, ctx, ens).at("ml_excursion_rr") ==
          Catch::Approx(std::tanh(100.0 / 40.0 - 1.0)));
}
