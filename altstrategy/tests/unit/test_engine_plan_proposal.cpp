// Trade-plan geometry + proposal grade/validity tests (§11, §12, §13, §41).

#include "altstrategy/engine/economic_policy.hpp"
#include "altstrategy/engine/proposal.hpp"
#include "altstrategy/engine/signal_predictions.hpp"
#include "altstrategy/engine/trade_plan.hpp"
#include "engine_test_helpers.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <set>

using namespace altstrategy::engine;
using namespace altstrategy::engine::test;

static StrategyCandidate long_candidate(const DecisionContext& ctx) {
    StrategyCandidate c;
    c.candidate_id = "cand-long";
    c.strategy_id = "momentum_continuation";
    c.strategy_version = "1.0.0";
    c.strategy_family = Family::trend;
    c.side = Side::long_;
    c.canonical_pair_id = ctx.canonical_pair_id;
    c.context_id = ctx.context_id;
    c.context_revision = ctx.context_revision;
    c.holding_class = HoldingClass::intraday;
    c.expected_horizon_ms = 4LL * 3600 * 1000;
    const double px = ctx.last_price();
    c.entry_hint = px; c.invalidation_hint = px * 0.99; c.target_hint = px * 1.02;
    c.adaptive_score = 0.75;
    c.static_score = 0.7;
    c.raw_setup_score = 0.7;
    c.lifecycle_status = Lifecycle::active;
    c.selection_mode = SelectionMode::exploit;
    c.reason_codes = {"ema_stack_aligned"};
    c.ml_context = ctx.market_forecast;
    // The market view on every horizon, and the economic DIAGNOSTIC from the
    // fixture's 4h estimate (+8 bps).
    c.predictions = market_predictions(ctx, 1'000'050, EconomicPolicyConfig{}.max_forecast_age_ms);
    c.descriptors = {{"break_distance_atr", 0.4}, {"room_to_zone_atr", 2.0}};
    c.econ = evaluate_economics(c.side, c.expected_horizon_ms, ctx, 1'000'050, {});
    return c;
}

TEST_CASE("plan generator emits AGGRESSIVE/BALANCED/CONSERVATIVE with valid long geometry") {
    auto ctx = make_ctx(100.0);
    const auto c = long_candidate(ctx);
    const auto plans = generate_plans(c, ctx, 2'000'000);
    REQUIRE(plans.size() == 3);
    std::set<PlanProfile> profiles;
    for (const auto& p : plans) {
        profiles.insert(p.profile);
        // Price-side invariants (long): stop < entry < target.
        CHECK(p.stop_loss_price < p.entry_price);
        CHECK(p.take_profit_price > p.entry_price);
        CHECK(p.expected_rr > 0);
        CHECK(p.max_holding_time_ms <= 48LL * 3600 * 1000);   // hard 48h invariant
        CHECK(p.time_stop_at_ms > 2'000'000);
        CHECK(p.entry_fill_timeout_ms > 0);
        CHECK(p.entry_fill_timeout_ms <= p.max_holding_time_ms);  // entry can't wait 48h
        CHECK(p.expected_fill_time_p90_min >= p.expected_fill_time_p50_min);
    }
    CHECK(profiles.size() == 3);
    // Immutable, distinct plan ids.
    CHECK(plans[0].variant_id != plans[1].variant_id);
}

TEST_CASE("conservative stop is wider than aggressive stop") {
    auto ctx = make_ctx(100.0);
    const auto plans = generate_plans(long_candidate(ctx), ctx, 2'000'000);
    double aggr = 0, cons = 0;
    for (const auto& p : plans) {
        if (p.profile == PlanProfile::aggressive) aggr = p.stop_distance_bps;
        if (p.profile == PlanProfile::conservative) cons = p.stop_distance_bps;
    }
    CHECK(cons > aggr);
}

TEST_CASE("SHORT geometry mirrors: stop > entry > target") {
    auto ctx = make_ctx(100.0, "TREND_DOWN");
    auto c = long_candidate(ctx);
    c.side = Side::short_;
    const double px = ctx.last_price();
    c.entry_hint = px; c.invalidation_hint = px * 1.01; c.target_hint = px * 0.98;
    const auto plans = generate_plans(c, ctx, 2'000'000);
    for (const auto& p : plans) {
        CHECK(p.stop_loss_price > p.entry_price);
        CHECK(p.take_profit_price < p.entry_price);
    }
}

TEST_CASE("tick-collapsed plan distances describe the final price geometry", "[plan][regression]") {
    const auto ctx = make_ctx(100.0);
    PlanConfig cfg;
    cfg.tick_size = 10.0; // Synthetic coarse grid collapses both initial barriers.
    for (const auto side : {Side::long_, Side::short_}) {
        auto c = long_candidate(ctx);
        c.side = side;
        const auto plans = generate_plans(c, ctx, 2'000'000, cfg);
        REQUIRE(plans.size() == 3);
        for (const auto& p : plans) {
            REQUIRE(p.entry_price > 0.0);
            CHECK(p.target_distance_bps == Catch::Approx(std::abs(p.take_profit_price - p.entry_price) / p.entry_price * 10000.0));
            CHECK(p.stop_distance_bps == Catch::Approx(std::abs(p.stop_loss_price - p.entry_price) / p.entry_price * 10000.0));
            CHECK(p.stop_distance_atr == Catch::Approx(std::abs(p.stop_loss_price - p.entry_price) / ctx.bitget_features.get("atr_14")));
            CHECK(p.target_distance_bps > 0.0);
            CHECK(p.stop_distance_bps > 0.0);
        }
    }
}

TEST_CASE("a signal with a complete ACTIVE prediction is NORMAL; VPS1 recommends nothing for execution",
          "[proposal][vps1][regression]") {
    auto ctx = make_ctx(100.0);
    const auto c = long_candidate(ctx);
    REQUIRE(c.econ.state == EconState::positive_edge);
    REQUIRE(c.econ.horizon_minutes == 240);
    const auto plans = generate_plans(c, ctx, 2'000'000);
    MlEvaluation ml;
    EnsembleView ens; ens.ensemble_direction = Side::long_; ens.ensemble_agreement = 0.8;
    ProposalInputs in{c, plans[1], ml, ens, ctx, "rank-1", {"plan-a", "plan-c"}, true, 1};
    const auto s = build_proposal(in, 2'000'000);
    CHECK(s.proposal_grade == Grade::normal);     // the market view is complete
    CHECK_FALSE(s.recommended_for_execution);     // never VPS1's decision, even on a positive diagnostic
    // The signal's content: all three horizons, the setup's description, the
    // horizon matching the setup's own.
    REQUIRE(s.market_predictions.size() == 3);
    CHECK(s.market_predictions[0].horizon_minutes == 15);
    CHECK(s.market_predictions[2].horizon_minutes == 240);
    for (const auto& h : s.market_predictions) {
        CHECK(h.state == PredictionState::present);
        CHECK(h.role == "ACTIVE");
        CHECK(h.p_up + h.p_flat + h.p_down == Catch::Approx(1.0));
    }
    CHECK(s.evaluation_horizon_minutes == 240);
    CHECK(s.setup_descriptors.at("room_to_zone_atr") == 2.0);
    CHECK(s.signal_semantics == "vps1-signal-1");
    CHECK(s.model_versions.size() == 3);
    // The economics stay on the record, as a diagnostic: +8 bps net over the
    // 4h horizon, stated at severity "info", never a risk flag.
    CHECK(s.edge_estimated);
    CHECK(s.expected_net_return == Catch::Approx(0.0008));
    bool diag = false;
    for (const auto& w : s.warnings)
        if (w.code == "ECON_DIAG_POSITIVE_EDGE") { diag = true; CHECK(w.severity == "info"); }
    CHECK(diag);
    for (const auto& f : s.risk_flags) CHECK(f.rfind("ECON_DIAG_", 0) != 0);
    CHECK_FALSE(s.p_target_estimated);
    CHECK(s.state == ProposalState::proposed);
    CHECK(s.take_profit_price > s.entry_price);
    CHECK(s.max_holding_time_ms <= 48LL * 3600 * 1000);
    CHECK(s.time_stop_at_ms > s.created_at_ms);
    CHECK_FALSE(s.signal_proposal_id.empty());
    CHECK(s.alternative_plan_ids.size() == 2);
}

TEST_CASE("technically INVALID geometry is graded INVALID and never executable (§13)") {
    auto ctx = make_ctx(100.0);
    const auto c = long_candidate(ctx);
    auto plans = generate_plans(c, ctx, 2'000'000);
    auto bad = plans[1];
    bad.take_profit_price = bad.entry_price - 1.0;   // LONG TP <= entry
    MlEvaluation ml; ml.present = true;
    EnsembleView ens; ens.ensemble_direction = Side::long_;
    ProposalInputs in{c, bad, ml, ens, ctx, "rank-1", {}, true, 1};
    const auto s = build_proposal(in, 2'000'000);
    CHECK(s.proposal_grade == Grade::invalid);
    CHECK_FALSE(s.recommended_for_execution);
    CHECK(s.state != ProposalState::proposed);
    const auto reasons = invalid_reasons(bad, ctx, 2'000'000);
    bool has = false; for (const auto& r : reasons) if (r == "LONG_TP_LE_ENTRY") has = true;
    CHECK(has);
}

TEST_CASE("expired context is INVALID") {
    auto ctx = make_ctx(100.0);
    ctx.expires_at_ms = 1'500'000;
    const auto c = long_candidate(ctx);
    const auto plans = generate_plans(c, ctx, 2'000'000);
    const auto reasons = invalid_reasons(plans[1], ctx, 2'000'000);   // now > expires
    bool expired = false; for (const auto& r : reasons) if (r == "EXPIRED_CONTEXT") expired = true;
    CHECK(expired);
}

TEST_CASE("a signal with no model is still a valid published signal, graded WEAK",
          "[proposal][vps1][regression]") {
    auto ctx = make_ctx(100.0);
    ctx.forecasts.clear();                          // AltML down: no forecast on any horizon
    auto c = long_candidate(ctx);
    const auto plans = generate_plans(c, ctx, 2'000'000);
    MlEvaluation ml;
    EnsembleView ens; ens.ensemble_direction = Side::long_;
    ProposalInputs in{c, plans[1], ml, ens, ctx, "rank-1", {}, false, 1};
    const auto s = build_proposal(in, 2'000'000);
    CHECK(s.proposal_grade == Grade::weak);
    CHECK_FALSE(s.recommended_for_execution);
    CHECK(s.state == ProposalState::proposed);   // still a valid published signal
    CHECK_FALSE(s.edge_estimated);               // nobody estimated anything
    REQUIRE(s.market_predictions.size() == 3);
    for (const auto& h : s.market_predictions) {
        CHECK(h.state == PredictionState::missing);
        CHECK_FALSE(h.has_values());
    }
    bool ml_missing = false;
    for (const auto& w : s.warnings) if (w.code == "MODEL_UNAVAILABLE") ml_missing = true;
    CHECK(ml_missing);
    CHECK(s.model_versions.empty());
}

TEST_CASE("a strategy's lifecycle no longer grades or suppresses its events",
          "[proposal][vps1][regression]") {
    // Lifecycle demotions come from a strategy's realised PnL as a TRADER. A
    // bad historical trader can be an informative event detector, so a
    // demoted detector's signal is graded like any other: by how complete its
    // market view is. None is recommended for execution.
    for (const auto status : {Lifecycle::degraded, Lifecycle::candidate, Lifecycle::limited,
                              Lifecycle::backtested, Lifecycle::shadow, Lifecycle::research,
                              Lifecycle::active}) {
        auto ctx = make_ctx(100.0);
        auto c = long_candidate(ctx);
        c.lifecycle_status = status;
        c.selection_mode = SelectionMode::explore_shadow;
        const auto plans = generate_plans(c, ctx, 2'000'000);
        MlEvaluation ml;
        EnsembleView ens; ens.ensemble_direction = Side::long_; ens.ensemble_agreement = 0.9;
        ProposalInputs in{c, plans[1], ml, ens, ctx, "rank-1", {}, true, 1};
        const auto s = build_proposal(in, 2'000'000);
        INFO(to_string(status));
        CHECK(s.proposal_grade == Grade::normal);
        CHECK_FALSE(s.recommended_for_execution);
    }
}

TEST_CASE("the 48-hour ceiling cannot be raised by a PlanConfig") {
    // The ceiling used to live in three places: kMaxHoldingMs, a local k48h in
    // proposal.cpp, and PlanConfig::extended_max_ms -- and the last was read
    // from `trade_plan.max_holding_hours` with no upper bound. A config could
    // therefore raise the "hard invariant". application.cpp now refuses such a
    // config at startup, and generate_plans bounds by the constant regardless,
    // which is what this pins.
    auto ctx = make_ctx(100.0);
    const auto c = long_candidate(ctx);
    PlanConfig cfg;
    cfg.extended_max_ms = 200LL * 3600 * 1000;   // a caller asking for 200 hours
    const auto plans = generate_plans(c, ctx, 2'000'000, cfg);
    REQUIRE_FALSE(plans.empty());
    for (const auto& v : plans) {
        INFO("variant " << v.variant_id);
        CHECK(v.max_holding_time_ms <= kMaxHoldingMs);
        CHECK(v.time_stop_at_ms <= 2'000'000 + kMaxHoldingMs);
    }
}

TEST_CASE("a warning states the observed value and the threshold it crossed") {
    // "wide spread" told an operator nothing: not how wide, not against what
    // bound, and not whether the reading was measured or the conservative
    // stand-in used when the book is unavailable.
    auto ctx = make_ctx(100.0);
    ctx.spread_bps = 25.0;
    ctx.spread_observed = false;          // the stand-in, not a measurement
    const auto c = long_candidate(ctx);
    const auto plans = generate_plans(c, ctx, 2'000'000);
    MlEvaluation ml;
    ml.unavailable = true;
    EnsembleView ens;
    ProposalInputs in{c, plans[0], ml, ens, ctx, "rank-w", {}, true, 1};
    const auto s = build_proposal(in, 2'000'000);

    bool found = false;
    for (const auto& w : s.warnings) {
        if (w.code != "WIDE_SPREAD") continue;
        found = true;
        INFO("detail: " << w.detail);
        CHECK(w.detail.find("25.00") != std::string::npos);   // observed
        CHECK(w.detail.find("8.00") != std::string::npos);    // threshold
        CHECK(w.detail.find("UNOBSERVED") != std::string::npos);
    }
    CHECK(found);
}

TEST_CASE("a worse book never estimates a faster passive fill") {
    // estimate_fill() is written for a [0,1] liquidity score, and generate_plans()
    // handed it AltTech's ORDINAL rank 1..4: rank 1 gave a factor of 0.5 and
    // every other rank the 0.4 floor, so a worse book estimated a FASTER fill
    // than the best one.
    const auto passive_p50 = [](double rank) {
        auto ctx = make_ctx();
        ctx.liquidity_rank = rank;
        std::optional<double> p50;
        for (const auto& v : generate_plans(long_candidate(ctx), ctx, 1'000'000))
            if (v.entry_type == EntryType::limit) p50 = v.expected_fill_time_p50_min;
        REQUIRE(p50.has_value());
        return *p50;
    };
    const double best = passive_p50(1.0), second = passive_p50(2.0), worst = passive_p50(4.0);
    CHECK(worst > best);
    CHECK(second > best);
    CHECK(second < worst);
    // Absent is charged the midpoint, as the slippage term is.
    const double absent = passive_p50(0.0);
    CHECK(absent > best);
    CHECK(absent < worst);
}


TEST_CASE("the economic verdict is a diagnostic: every state publishes, none recommends",
          "[policy][vps1][regression]") {
    auto ctx = make_ctx(100.0);
    EnsembleView ens; ens.ensemble_direction = Side::long_; ens.ensemble_agreement = 0.8;
    MlEvaluation ml;
    struct Case { EconState state; const char* warning; const char* severity; };
    const Case cases[] = {
        {EconState::no_edge, "ECON_DIAG_NO_EDGE", "info"},
        {EconState::small_edge, "ECON_DIAG_SMALL_EDGE", "info"},
        {EconState::strong_edge, "ECON_DIAG_STRONG_EDGE", "info"},
        {EconState::horizon_conflict, "ECON_DIAG_HORIZON_CONFLICT", "info"},
        {EconState::model_unavailable, "MODEL_UNAVAILABLE", "warning"},
        {EconState::model_stale, "MODEL_STALE", "warning"},
        {EconState::model_abstain, "MODEL_ABSTAIN", "warning"},
        {EconState::no_data, "MODEL_INCOMPLETE_INPUT", "warning"},
        {EconState::model_shadow, "MODEL_SHADOW", "warning"},
    };
    for (const auto& k : cases) {
        auto c = long_candidate(ctx);
        c.econ.state = k.state;
        const auto plans = generate_plans(c, ctx, 2'000'000);
        ProposalInputs in{c, plans[1], ml, ens, ctx, "rank-1", {}, true, 1};
        const auto s = build_proposal(in, 2'000'000);
        INFO("state " << to_string(k.state));
        CHECK(s.proposal_grade != Grade::invalid);
        CHECK(s.state == ProposalState::proposed);
        CHECK_FALSE(s.recommended_for_execution);
        bool warned = false;
        for (const auto& w : s.warnings)
            if (w.code == k.warning) { warned = true; CHECK(w.severity == k.severity); }
        CHECK(warned);
        CHECK(s.risk_flags.empty());   // no diagnostic or prediction condition is a risk flag
    }
}

TEST_CASE("a short hypothesis is never refused for an entry timeout longer than its window",
          "[plan][vps1][regression]") {
    // A thin book stretched the passive fill estimate past a scalp setup's
    // holding window; the validity check then refused the signal itself.
    auto ctx = make_ctx(100.0);
    ctx.top_depth_usdt = 1'000;                         // thin
    ctx.microstructure.values["trade_intensity"] = 0.05;  // almost no tape
    auto c = long_candidate(ctx);
    c.strategy_id = "book_imbalance_pressure";
    c.holding_class = HoldingClass::scalp;
    c.expected_horizon_ms = 20LL * 60 * 1000;
    const auto plans = generate_plans(c, ctx, 2'000'000);
    REQUIRE_FALSE(plans.empty());
    for (const auto& v : plans) {
        CHECK(v.entry_fill_timeout_ms <= v.max_holding_time_ms);
        CHECK(v.entry_fill_timeout_ms > 0);
        const auto reasons = invalid_reasons(v, ctx, 2'000'000);
        for (const auto& r : reasons) CHECK(r != "ENTRY_TIMEOUT_EXCEEDS_HOLDING");
    }
}
