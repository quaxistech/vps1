// Catalogue + strategy_engine tests (§8 catalogue, §41 strategy/family tests,
// plus the momentum_continuation / trend_pullback regression).

#include "altstrategy/engine/strategy_engine.hpp"
#include "engine_test_helpers.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>

using namespace altstrategy::engine;
using namespace altstrategy::engine::test;

TEST_CASE("catalogue holds the sixteen specified strategies with distinct families") {
    StrategyEngine eng;
    const auto metas = eng.metas();
    REQUIRE(metas.size() == 16);

    std::set<std::string> ids;
    std::set<Family> families;
    for (const auto& m : metas) { ids.insert(m.strategy_id); families.insert(m.family); }
    for (const char* id : {"momentum_continuation", "trend_pullback",
                           "multi_timeframe_momentum_alignment", "volume_weighted_momentum_burst",
                           "donchian_breakout_continuation", "ema_compression_expansion",
                           "cross_venue_lead_lag_follow", "vwap_reclaim_continuation",
                           "range_extreme_mean_reversion", "funding_oi_crowding_unwind",
                           "book_imbalance_pressure", "cvd_absorption_reversal",
                           "spread_dislocation_fade", "funding_basis_convergence",
                           "venue_dislocation_convergence", "htf_range_false_breakout_fade"})
        CHECK(ids.count(id) == 1);
    // Ten families. Family is the unit the ensemble measures independence over,
    // so adding hypotheses inside one family adds correlated opinions, not
    // information -- six of the original ten sat in trend/breakout alone.
    for (Family fam : {Family::trend, Family::breakout, Family::mean_reversion,
                       Family::cross_venue, Family::vwap, Family::crowding,
                       Family::microstructure, Family::order_flow, Family::basis,
                       Family::liquidity})
        CHECK(families.count(fam) == 1);
}

TEST_CASE("all sixteen strategies refuse absent or zero true ATR", "[catalogue][quality]") {
    StrategyEngine engine;
    auto ctx = make_ctx();
    ctx.cross_venue_features.values["atr_14"] = 1.0; // cannot stand in for Bitget ATR
    ctx.microstructure.values["atr_14"] = 1.0;
    for (const auto& meta : engine.metas()) {
        CAPTURE(meta.strategy_id);
        for (const double atr : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
            ctx.bitget_features.values["atr_14"] = atr;
            std::string reason;
            CHECK_FALSE(StrategyEngine::eligible(meta, Lifecycle::active, ctx, &reason));
            CHECK(reason == "missing_feature");
        }
    }
}

TEST_CASE("invalid prerequisite source cannot borrow another block's valid flag", "[catalogue][quality]") {
    StrategyEngine engine;
    auto ctx = make_ctx();
    ctx.bitget_features.valid = false;
    for (const auto& meta : engine.metas()) {
        std::string reason;
        CAPTURE(meta.strategy_id);
        CHECK_FALSE(StrategyEngine::eligible(meta, Lifecycle::active, ctx, &reason));
        CHECK(reason == "missing_feature");
    }
}

TEST_CASE("candidate validity never extends source context expiry", "[catalogue][ttl]") {
    StrategyEngine engine;
    auto ctx = make_ctx();
    constexpr int64_t now = 1'000'050;
    ctx.expires_at_ms = now + 10;
    const auto candidates = engine.run(ctx, now, {});
    REQUIRE_FALSE(candidates.empty());
    for (const auto& candidate : candidates) CHECK(candidate.expires_at_ms == now + 10);
    CHECK(engine.run(ctx, now + 10, {}).empty());
}

TEST_CASE("range rejection is measured by position in the range, not by momentum") {
    // The hypothesis is a rejection AT a range boundary. It was operationalised
    // as RSI(14) > 72 / < 28 -- fourteen bars of MOMENTUM, which is exactly what
    // takes a market out of RANGE. AltTech's classifier never reads RSI, but it
    // reads ADX and Bollinger width, and a run long enough to push RSI past 72
    // pushes ADX past 25 (TREND) or the band percentile past 0.90 (BREAKOUT).
    // Measured over seven days: 427 contexts had an RSI extreme, 0 of 1124 RANGE
    // contexts among them; the five that also had ADX < 18 came out BREAKOUT (3)
    // and TREND_UP (2). Where price actually sat at the edge of the 20-bar range
    // inside RANGE, RSI averaged 54.3 at the top fifth and 47.0 at the bottom.
    StrategyEngine eng;
    auto ctx = make_ctx();
    // The range is the 15m grid's own regime -- the grid the channel is on.
    ctx.bitget_htf_regime[15].regime = "RANGE";
    ctx.bitget_features.values["adx_14"] = 12.0;        // no trend
    ctx.bitget_features.values["atr_expansion"] = 1.0;  // no expansion
    ctx.bitget_features.values["rsi_14"] = 54.0;        // mid-range, as measured
    ctx.microstructure.values["trade_imbalance"] = -0.1;   // sellers, fading a high
    // The ML veto is a separate, correct rule: a forecast of continuation
    // THROUGH the boundary cancels the fade. The fixture ships p_up = 0.58, so
    // it is neutralised here to test the boundary predicate on its own.
    ctx.market_forecast.p_up = 0.40;
    ctx.market_forecast.p_down = 0.45;

    const double px = ctx.last_price();
    ProjectedZone z;
    z.kind = "resistance";
    z.center = px * 1.001;
    ctx.bitget_projected_zones.push_back(z);

    const auto fires = [&]() {
        for (const auto& c : eng.run(ctx, 1'000'000, {})) {
            if (c.strategy_id == "range_extreme_mean_reversion") return true;
        }
        return false;
    };

    // Price in the top fifth of the 20-bar range, RSI at 54: the hypothesis
    // applies and the strategy now sees it.
    ctx.bitget_features.values["donchian_low_20"] = px * 0.98;
    ctx.bitget_features.values["donchian_high_20"] = px * 1.001;
    CHECK(fires());

    // Mid-range: not a boundary, no candidate however the momentum looks.
    ctx.bitget_features.values["donchian_low_20"] = px * 0.95;
    ctx.bitget_features.values["donchian_high_20"] = px * 1.05;
    CHECK_FALSE(fires());

    // A degenerate range cannot state a position at all.
    ctx.bitget_features.values["donchian_low_20"] = px;
    ctx.bitget_features.values["donchian_high_20"] = px;
    CHECK_FALSE(fires());

    // The 1m regimes say nothing about a 15m range: TREND_UP on both 1m
    // regimes no longer suppresses the setup, a non-RANGE 15m regime does.
    ctx.bitget_features.values["donchian_low_20"] = px * 0.98;
    ctx.bitget_features.values["donchian_high_20"] = px * 1.001;
    ctx.global_regime.regime = "TREND_UP";
    ctx.bitget_regime.regime = "TREND_UP";
    CHECK(fires());
    ctx.bitget_htf_regime[15].regime = "TREND_UP";
    CHECK_FALSE(fires());
}

TEST_CASE("multi-timeframe alignment reads three grids, and refuses on two") {
    // The strategy read mom_short/mom_mid/mom_long all from the SAME execution
    // vector -- ROC over 5, 14 and 20 bars of one grid, which on 15m is 75/210/
    // 300 minutes, not the "1m/5m/15m" its explanation claimed. The higher grids
    // the assembler admits were read by nothing, which ablation showed exactly:
    // keeping only 30m, only 1h or only 1d gave byte-identical results.
    StrategyEngine eng;
    auto ctx = make_ctx();
    ctx.global_regime.regime = "TREND_UP";
    ctx.bitget_regime.regime = "TREND_UP";
    ctx.cross_venue_features.values["momentum_direction_agreement"] = 0.5;
    ctx.bitget_features.values["mom_short"] = 0.004;
    ctx.bitget_features.values["vwap"] = ctx.last_price() * 0.99;   // price above vwap

    const auto grid = [](double roc_14) {
        FeatureSummary s;
        s.valid = true;
        s.values = {{"roc_14", roc_14}};
        return s;
    };
    const auto produces = [&]() {
        for (const auto& c : eng.run(ctx, 1'000'000, {})) {
            if (c.strategy_id == "multi_timeframe_momentum_alignment") return true;
        }
        return false;
    };

    // Only one higher grid present: an alignment across three cannot be
    // asserted from two, so there is no candidate rather than a guessed one.
    ctx.context_features[30] = grid(0.003);
    CHECK_FALSE(produces());

    // Both grids present and agreeing with the execution grid.
    ctx.context_features[60] = grid(0.002);
    CHECK(produces());

    // A higher grid pointing the other way breaks the alignment.
    ctx.context_features[60] = grid(-0.002);
    CHECK_FALSE(produces());
}

TEST_CASE("a prerequisite is checked where the strategy will actually read") {
    // feature_present() searched the information venues too, so "is atr_14
    // present?" could be answered by Binance's atr_14 while the strategy went on
    // to read ctx.bitget_features and get the DEFAULT from an empty field -- a
    // missing input arriving as a number, past its own prerequisite.
    //
    // Nothing in the catalogue reads ctx.reference_features; that loop was its
    // only use in the engine. Ablation showed the consequence directly: keeping
    // only Binance, only OKX or only Bybit gave byte-identical results.
    StrategyEngine eng;
    auto ctx = make_ctx();
    ctx.global_regime.regime = "TREND_UP";
    ctx.bitget_regime.regime = "TREND_UP";

    // Strip the execution venue's indicators, then offer them from an
    // information venue instead.
    ctx.bitget_features.values.clear();
    FeatureSummary binance;
    binance.valid = true;
    binance.values = {{"ema_20", 100.0}, {"ema_50", 99.0}, {"atr_14", 1.0},
                      {"adx_14", 30.0}, {"rsi_14", 55.0}};
    ctx.reference_features[2] = binance;

    // No candidate may be produced from another venue's readings.
    const auto candidates = eng.run(ctx, 1'000'000, {});
    CHECK(candidates.empty());
}

TEST_CASE("dislocation is Bitget against the consensus, not the consensus against itself") {
    // venue_dislocation_convergence computed (last_price - consensus_mid) /
    // consensus_mid. last_price() returns mark_price, and the assembler fills
    // mark_price FROM consensus_mid_price -- so the gap was the consensus
    // compared with itself: identically zero on every context, and the >= 4 bps
    // gate could never open. Zero candidates for the strategy's whole deployed
    // life, with nothing in any log to say why.
    StrategyEngine eng;
    auto ctx = make_ctx();
    ctx.global_regime.regime = "RANGE";
    ctx.bitget_regime.regime = "RANGE";
    ctx.cross_venue_features.values["consensus_mid_price"] = ctx.last_price();
    ctx.cross_venue_features.values["venue_price_dispersion_bps"] = 2.0;
    ctx.cross_venue_features.values["bitget_lead_score"] = 0.0;   // Bitget is not leading

    const auto has_dislocation_candidate = [&](double deviation_bps) {
        ctx.cross_venue_features.values["bitget_mid_deviation_bps"] = deviation_bps;
        for (const auto& c : eng.run(ctx, 1'000'000, {})) {
            if (c.strategy_id == "venue_dislocation_convergence") return true;
        }
        return false;
    };

    // Below the 4 bps gate: correctly silent.
    CHECK_FALSE(has_dislocation_candidate(1.0));
    // Bitget 9 bps ABOVE the consensus: expect convergence downward.
    CHECK(has_dislocation_candidate(9.0));
    // And symmetrically below.
    CHECK(has_dislocation_candidate(-9.0));
}

TEST_CASE("every strategy declares a horizon inside the 48-hour ceiling") {
    StrategyEngine eng;
    for (const auto& m : eng.metas()) {
        INFO("strategy: " << m.strategy_id);
        CHECK_FALSE(m.holding_classes.empty());
        CHECK_FALSE(m.explanation_template.empty());
        // A parameter with min == max cannot be calibrated and is a constant
        // wearing a parameter's name.
        for (const auto& prm : m.parameter_schema) {
            INFO("param: " << prm.name);
            CHECK(prm.max > prm.min);
            CHECK(prm.default_value >= prm.min);
            CHECK(prm.default_value <= prm.max);
        }
    }
}

TEST_CASE("initial lifecycle seeds match the specification (§8/§13)") {
    StrategyEngine eng;
    std::map<std::string, Lifecycle> seed;
    for (const auto& m : eng.metas()) seed[m.strategy_id] = m.default_lifecycle;
    CHECK(seed["momentum_continuation"] == Lifecycle::active);
    // CANDIDATE since the ML-first policy: redefined as a 1h-regime + 15m
    // pullback setup whose edge is not established out of sample.
    CHECK(seed["trend_pullback"] == Lifecycle::candidate);
    CHECK(seed["multi_timeframe_momentum_alignment"] == Lifecycle::candidate);
    CHECK(seed["volume_weighted_momentum_burst"] == Lifecycle::candidate);
    CHECK(seed["donchian_breakout_continuation"] == Lifecycle::shadow);
    CHECK(seed["cross_venue_lead_lag_follow"] == Lifecycle::shadow);
    CHECK(seed["range_extreme_mean_reversion"] == Lifecycle::research);
    CHECK(seed["funding_oi_crowding_unwind"] == Lifecycle::research);
}

TEST_CASE("momentum_continuation regression: LONG in an aligned up-trend") {
    StrategyEngine eng;
    auto ctx = make_ctx(100.0, "TREND_UP");
    const auto cands = eng.run(ctx, 1'000'100, {});
    bool found = false;
    for (const auto& c : cands)
        if (c.strategy_id == "momentum_continuation") {
            found = true;
            CHECK(c.side == Side::long_);
            CHECK(c.raw_rr() > 0);            // valid geometry
            CHECK(c.static_score > 0);
            CHECK(c.context_revision == 7);   // lineage preserved
            CHECK(!c.reason_codes.empty());
        }
    CHECK(found);
}

TEST_CASE("momentum_continuation regression: SHORT mirrors in a down-trend") {
    StrategyEngine eng;
    auto ctx = make_ctx(100.0, "TREND_DOWN");
    // Flip the EMA stack + momentum for a short setup.
    ctx.bitget_features.values["ema_20"] = 101.0;   // wait: below price for short
    ctx.bitget_features.values["ema_20"] = 100.5;
    ctx.bitget_features.values["ema_50"] = 102.0;
    ctx.mark_price = 99.0;
    ctx.bitget_features.values["close"] = 99.0;
    ctx.cross_venue_features.values["momentum_direction_agreement"] = -0.4;
    ctx.market_forecast.p_up = 0.25; ctx.market_forecast.p_down = 0.58;
    const auto cands = eng.run(ctx, 1'000'100, {});
    bool found = false;
    for (const auto& c : cands)
        if (c.strategy_id == "momentum_continuation") { found = true; CHECK(c.side == Side::short_); }
    CHECK(found);
}

TEST_CASE("momentum continuation needs a 1h trend statement, not a permitted 1m regime") {
    // No regime gates eligibility any more; the setup's own definition asks
    // for a TREND on Bitget's 1h grid, and CHAOTIC there is no trend.
    StrategyEngine eng;
    auto ctx = make_ctx(100.0, "CHAOTIC");
    const auto cands = eng.run(ctx, 1'000'100, {});
    for (const auto& c : cands) CHECK(c.strategy_id != "momentum_continuation");
    // A 1h trend fires it whatever the 1m regimes say.
    ctx.bitget_htf_regime[60].regime = "TREND_UP";
    bool fired = false;
    for (const auto& c : eng.run(ctx, 1'000'100, {})) fired |= c.strategy_id == "momentum_continuation";
    CHECK(fired);
}

TEST_CASE("missing required feature makes a strategy ineligible") {
    StrategyEngine eng;
    auto ctx = make_ctx();
    ctx.bitget_features.values.erase("adx_14");
    ctx.cross_venue_features.values.erase("atr_14");   // adx_14 required by momentum
    std::string reason;
    StrategyMeta m;
    m.strategy_id = "x"; m.required_features = {"adx_14"};
    CHECK_FALSE(StrategyEngine::eligible(m, Lifecycle::active, ctx, &reason));
    CHECK(reason == "missing_feature");
}

TEST_CASE("weak candidates are NOT hidden and INVALID context blocks eligibility") {
    StrategyEngine eng;
    auto ctx = make_ctx();
    ctx.data_quality = ContextQuality::invalid;
    const auto cands = eng.run(ctx, 1'000'100, {});
    CHECK(cands.empty());   // invalid context => no eligible strategies
}

TEST_CASE("shadow/research strategies default to EXPLORE_SHADOW selection mode") {
    StrategyEngine eng;
    auto ctx = make_ctx(100.0, "RANGE");
    // Range context so the mean-reversion (RESEARCH) strategy can trigger.
    ctx.bitget_regime.regime = "RANGE"; ctx.global_regime.regime = "RANGE";
    ctx.bitget_features.values["adx_14"] = 12;
    ctx.bitget_features.values["rsi_14"] = 75;
    ctx.bitget_features.values["atr_expansion"] = 1.0;
    ctx.microstructure.values["trade_imbalance"] = -0.1;
    ProjectedZone z; z.kind = "resistance"; z.center = 100.3; z.lower = 100.2; z.upper = 100.4;
    ctx.bitget_projected_zones.push_back(z);
    ctx.market_forecast.p_down = 0.3;
    const auto cands = eng.run(ctx, 1'000'100, {});
    for (const auto& c : cands)
        if (c.lifecycle_status == Lifecycle::research || c.lifecycle_status == Lifecycle::shadow)
            CHECK(c.selection_mode == SelectionMode::explore_shadow);
}

// ------------------------------------------------ holding invariant

TEST_CASE("no candidate may exceed the 48h holding ceiling") {
    // 48h is a safety ceiling, not a normal holding period. It used to live only
    // inside PlanConfig and was applied with a std::min() at plan construction,
    // so a strategy asking for longer was silently truncated rather than
    // refused -- the plan then executed a different thesis under the same name.
    StrategyEngine eng;
    auto ctx = make_ctx();
    const auto out = eng.run(ctx, 1'000'100, {});
    for (const auto& c : out) {
        INFO("strategy " << c.strategy_id);
        CHECK(c.expected_horizon_ms <= kMaxHoldingMs);
        CHECK(c.expected_horizon_ms > 0);
    }
}

TEST_CASE("every catalogued strategy declares a short-term horizon") {
    // The system is a short-term engine: a strategy whose own declared horizon
    // reaches the ceiling is not a scalp that drifted, it is a different kind of
    // system. This pins the catalogue's intent rather than one run's output.
    StrategyEngine eng;
    auto ctx = make_ctx();
    const auto out = eng.run(ctx, 1'000'100, {});
    for (const auto& c : out) {
        INFO("strategy " << c.strategy_id << " horizon_ms=" << c.expected_horizon_ms);
        // Most hypotheses should close far sooner than the ceiling; 24h is a
        // generous bound that still excludes swing/position behaviour.
        CHECK(c.expected_horizon_ms <= 24LL * 3600 * 1000);
    }
}

TEST_CASE("the declared holding class bounds the derived one") {
    // `clamp_holding` ignored the strategy's declared class entirely -- the
    // parameter was discarded with a `(void)h;` after every return -- so a
    // strategy calling itself a scalp could be promoted to an eight-hour hold
    // purely because its horizon drifted.
    StrategyEngine eng;
    auto ctx = make_ctx();
    const auto out = eng.run(ctx, 1'000'100, {});
    for (const auto& c : out) {
        INFO("strategy " << c.strategy_id);
        if (c.holding_class == HoldingClass::scalp) {
            CHECK(c.expected_horizon_ms <= 30LL * 60 * 1000);
        } else if (c.holding_class == HoldingClass::intraday) {
            CHECK(c.expected_horizon_ms <= 8LL * 3600 * 1000);
        }
    }
}

TEST_CASE("the spread fade no longer gates on the book's liquidity rank; it records it",
          "[strategies][vps1][regression]") {
    // The liquidity-rank floor ranked Bitget's book against the other venues'
    // -- an execution judgement (VPS2's). The event is the spread spike with
    // depth intact; the rank travels as a descriptor.
    StrategyEngine eng;
    auto ctx = make_ctx();
    ctx.spread_observed = true;
    ctx.depth_observed = true;
    ctx.spread_bps = 12.0;   // a spike past the 9 bps definition
    ctx.top_depth_usdt = 120'000;
    ctx.microstructure.values["spread_bps"] = 12.0;
    ctx.microstructure.values["book_bid_depth"] = 60'000;
    ctx.microstructure.values["book_ask_depth"] = 60'000;
    ctx.microstructure.values["book_microprice_dev_bps"] = 2.0;

    const auto find = [&](double rank) -> std::optional<StrategyCandidate> {
        ctx.liquidity_rank = rank;
        for (const auto& c : eng.run(ctx, 1'000'000, {}))
            if (c.strategy_id == "spread_dislocation_fade") return c;
        return std::nullopt;
    };
    for (const double rank : {1.0, 3.0, 4.0, 0.0}) {
        INFO(rank);
        const auto c = find(rank);
        REQUIRE(c.has_value());
        CHECK(c->descriptors.count("spread_bps") == 1);
        // Recorded when measured, absent when unknown -- never a stand-in 0.
        CHECK(c->descriptors.count("liquidity_score") == (rank >= 1.0 ? 1u : 0u));
    }
    // The definition still holds: no spike, no event.
    ctx.spread_bps = 4.0;
    CHECK_FALSE(find(1.0).has_value());
}

TEST_CASE("the higher-range mid is a target only on the trade's side of price") {
    // htf_range_false_breakout_fade: price broke the execution grid's high
    // while the 4h grid still ranges -> fade short. The comparisons were
    // inverted: a mid BELOW entry (where a short's target belongs) was never
    // used, and a mid ABOVE entry replaced the target, above a short's entry.
    StrategyEngine eng;
    auto ctx = make_ctx(100.0, "RANGE");
    const double px = ctx.last_price();
    const auto short_target = [&](double htf_lo, double htf_hi) -> std::optional<double> {
        FeatureSummary htf;
        htf.valid = true;
        htf.values = {{"adx_14", 15.0}, {"donchian_high_20", htf_hi}, {"donchian_low_20", htf_lo}};
        ctx.context_features[240] = htf;
        for (const auto& c : eng.run(ctx, 1'000'000, {}))
            if (c.strategy_id == "htf_range_false_breakout_fade") {
                REQUIRE(c.side == Side::short_);
                return c.target_hint;
            }
        return std::nullopt;
    };

    // Mid 99.0: below entry and nearer than the ATR target (100 - 1.8) -> it wins.
    const auto near = short_target(97.0, 101.0);
    REQUIRE(near.has_value());
    CHECK(*near == 99.0);
    // Mid 97.5: below entry but beyond the ATR target -> the nearer ATR target.
    const auto far = short_target(90.0, 105.0);
    REQUIRE(far.has_value());
    CHECK(*far == px - 1.8);
    // Mid 105: ABOVE a short's entry -> never a target.
    const auto wrong_side = short_target(90.0, 120.0);
    REQUIRE(wrong_side.has_value());
    CHECK(*wrong_side < px);
}

TEST_CASE("the funnel accounts for every context each strategy was offered") {
    StrategyEngine eng;
    auto ctx = make_ctx(100.0, "CHAOTIC");   // no regime gates anything any more
    // Two venues: the setups that need a genuine consensus (3 of 4) are
    // ineligible -- a data-quality requirement, the only kind left.
    ctx.data_quality = ContextQuality::degraded_2_of_4;
    StrategyFunnel funnel;
    const auto candidates = eng.run(ctx, 1'000'000, {}, &funnel);

    REQUIRE(funnel.by_strategy.size() == eng.size());
    int64_t produced = 0;
    for (const auto& [id, c] : funnel.by_strategy) {
        CAPTURE(id);
        CHECK(c.contexts == 1);
        int64_t ineligible = 0;
        for (const auto& [reason, n] : c.ineligible) ineligible += n;
        // Every context stops at exactly one stage.
        CHECK(ineligible + c.no_setup + c.geometry_rejected + c.horizon_refused + c.repeat_within_horizon + c.candidates ==
              c.contexts);
        CHECK(c.evaluated == c.contexts - ineligible);
        produced += c.candidates;
    }
    CHECK(produced == static_cast<int64_t>(candidates.size()));
    CHECK(funnel.by_strategy.at("spread_dislocation_fade").ineligible.at("quality_below_required") == 1);
    for (const auto& [id, c] : funnel.by_strategy) CHECK(c.ineligible.count("regime_not_allowed") == 0);

    // Windows add up.
    StrategyFunnel total;
    total.merge(funnel);
    total.merge(funnel);
    CHECK(total.by_strategy.at("spread_dislocation_fade").contexts == 2);
    CHECK(total.by_strategy.at("spread_dislocation_fade").ineligible.at("quality_below_required") == 2);
}

namespace {
bool fires(const StrategyEngine& eng, const DecisionContext& ctx, const char* id) {
    for (const auto& c : eng.run(ctx, 1'000'000, {}))
        if (c.strategy_id == id) return true;
    return false;
}
} // namespace

TEST_CASE("a Donchian breakout is a break of the channel the PREVIOUS bars made") {
    // TA-Lib MAX(high, 20) includes the bar that just closed, so "price above
    // the 20-bar high" measured against it needs price to clear the breakout
    // bar's own high after the close. The prior channel is the rule.
    StrategyEngine eng;
    auto ctx = make_ctx(100.0, "BREAKOUT");
    const double px = ctx.last_price();
    ctx.bitget_features.values["atr_expansion"] = 1.2;
    // The bar just closed AT its high: the inclusive channel's top is the price.
    ctx.bitget_features.values["donchian_high_20"] = px;
    // The 20 bars before it topped out 0.5% lower.
    ctx.bitget_features.values["donchian_high_20_prev"] = px * 0.995;
    ctx.bitget_features.values["donchian_low_20_prev"] = px * 0.95;
    CHECK(fires(eng, ctx, "donchian_breakout_continuation"));

    // Boundary: the noise bound is half the OBSERVED spread (it was a hand-set
    // 2 bps). With a measured 2 bps spread, a price exactly 1 bps past the
    // prior high is inside the quote, not a break; just past it, it is.
    ctx.spread_observed = true;
    ctx.spread_bps = 2.0;
    ctx.bitget_features.values["donchian_high_20_prev"] = px / 1.0001;
    CHECK_FALSE(fires(eng, ctx, "donchian_breakout_continuation"));
    ctx.bitget_features.values["donchian_high_20_prev"] = px / 1.00011;
    CHECK(fires(eng, ctx, "donchian_breakout_continuation"));
    // Unobserved spread: no bound can be derived, so a strict crossing.
    ctx.spread_observed = false;
    ctx.bitget_features.values["donchian_high_20_prev"] = px / 1.00001;
    CHECK(fires(eng, ctx, "donchian_breakout_continuation"));
    ctx.bitget_features.values["donchian_high_20_prev"] = px;
    CHECK_FALSE(fires(eng, ctx, "donchian_breakout_continuation"));

    // Inside the prior channel: no break, whatever the inclusive channel says.
    ctx.bitget_features.values["donchian_high_20_prev"] = px * 1.01;
    CHECK_FALSE(fires(eng, ctx, "donchian_breakout_continuation"));

    // No prior bar (restart, gap): no channel to break, so no candidate.
    ctx.bitget_features.values.erase("donchian_high_20_prev");
    ctx.bitget_features.values.erase("donchian_low_20_prev");
    CHECK_FALSE(fires(eng, ctx, "donchian_breakout_continuation"));
}

TEST_CASE("breakout: the setup is the break; expansion evidence is an input, not a gate") {
    // The donchian "expansion OR volume" confirmation was a filter on a setup
    // already defined by the close beyond the prior channel; the model reads
    // both quantities. A break fires whatever they say -- including when the
    // volume reading is absent, which is never turned into a number.
    StrategyEngine eng;
    auto ctx = make_ctx(100.0, "BREAKOUT");
    const double px = ctx.last_price();
    ctx.bitget_features.values["donchian_high_20_prev"] = px * 0.995;
    ctx.bitget_features.values["atr_expansion"] = 0.9;   // contracting
    ctx.microstructure.values["volume_surge"] = 0.5;     // no volume
    CHECK(fires(eng, ctx, "donchian_breakout_continuation"));
    ctx.microstructure.values.erase("volume_surge");     // volume unknown
    CHECK(fires(eng, ctx, "donchian_breakout_continuation"));
    // No break, no setup.
    ctx.bitget_features.values["donchian_high_20_prev"] = px * 1.01;
    CHECK_FALSE(fires(eng, ctx, "donchian_breakout_continuation"));

    // ema_compression_expansion: the same gate at 1.1.
    auto e = make_ctx(100.0, "TREND_UP");
    e.bitget_features.values["ema_compression"] = 0.002;
    e.bitget_features.values["atr_expansion"] = 1.05;
    e.microstructure.values.erase("volume_surge");
    CHECK_FALSE(fires(eng, e, "ema_compression_expansion"));
    e.microstructure.values["volume_surge"] = 1.2;
    CHECK(fires(eng, e, "ema_compression_expansion"));
}

TEST_CASE("the VWAP reclaim is measured against the session VWAP") {
    StrategyEngine eng;
    auto ctx = make_ctx(100.0, "RANGE");
    ctx.bitget_features.values["session_vwap"] = 99.9;
    ctx.bitget_features.values["session_vwap_dev_bps"] = 10.0;   // 10 bps above
    ctx.bitget_features.values["session_vwap_lost_recently"] = 1.0;
    ctx.microstructure.values["cvd_delta"] = 3.0;
    CHECK(fires(eng, ctx, "vwap_reclaim_continuation"));

    // Boundary: beyond the VWAP by no more than quote noise (half the observed
    // spread; a hand-set 3 bps stood here) is not a reclaim.
    ctx.spread_observed = true;
    ctx.spread_bps = 6.0;
    ctx.bitget_features.values["session_vwap_dev_bps"] = 3.0;
    CHECK_FALSE(fires(eng, ctx, "vwap_reclaim_continuation"));
    ctx.bitget_features.values["session_vwap_dev_bps"] = 10.0;
    CHECK(fires(eng, ctx, "vwap_reclaim_continuation"));

    // No recent crossing: price above the session VWAP all along is a trend.
    ctx.bitget_features.values["session_vwap_lost_recently"] = 0.0;
    CHECK_FALSE(fires(eng, ctx, "vwap_reclaim_continuation"));
    ctx.bitget_features.values["session_vwap_lost_recently"] = 1.0;

    // CVD is a confirmation of whether to TRADE the reclaim: recorded, not
    // required. A missing reading is simply absent from the descriptors.
    ctx.microstructure.values["cvd_delta"] = -3.0;
    CHECK(fires(eng, ctx, "vwap_reclaim_continuation"));
    ctx.microstructure.values.erase("cvd_delta");
    for (const auto& c : eng.run(ctx, 1'000'000, {}))
        if (c.strategy_id == "vwap_reclaim_continuation") CHECK(c.descriptors.count("cvd_delta") == 0);

    // A 1-minute window VWAP is not a session VWAP: without the session level
    // the strategy is not eligible, whatever the minute VWAP says.
    ctx.bitget_features.values.erase("session_vwap");
    ctx.bitget_features.values.erase("session_vwap_dev_bps");
    ctx.bitget_features.values["vwap"] = 99.9;
    ctx.bitget_features.values["vwap_dev"] = 10.0;
    ctx.bitget_features.values["vwap_lost_recently"] = 1.0;
    CHECK_FALSE(fires(eng, ctx, "vwap_reclaim_continuation"));
}

TEST_CASE("multi-timeframe alignment records the session VWAP side, with no EMA stand-in",
          "[strategies][vps1]") {
    // The VWAP side was a confirmation of whether to trade the alignment; the
    // alignment itself is three grids' momentum and two grids' regimes.
    StrategyEngine eng;
    auto ctx = make_ctx();
    ctx.cross_venue_features.values["momentum_direction_agreement"] = 0.5;
    ctx.bitget_features.values["mom_short"] = 0.004;
    FeatureSummary g;
    g.valid = true;
    g.values = {{"roc_14", 0.002}};
    ctx.context_features[30] = g;
    ctx.context_features[60] = g;
    const double px = ctx.last_price();
    const auto dev = [&]() -> std::optional<double> {
        for (const auto& c : eng.run(ctx, 1'000'000, {}))
            if (c.strategy_id == "multi_timeframe_momentum_alignment") {
                const auto it = c.descriptors.find("session_vwap_dev_bps");
                return it == c.descriptors.end() ? std::optional<double>(NAN) : std::optional<double>(it->second);
            }
        return std::nullopt;
    };
    ctx.bitget_features.values["session_vwap"] = px * 0.99;   // price above
    REQUIRE(dev().has_value());
    CHECK(*dev() > 0);
    ctx.bitget_features.values["session_vwap"] = px * 1.01;   // price below: still the event
    REQUIRE(dev().has_value());
    CHECK(*dev() < 0);
    // Absent: nothing stands in for it (EMA(20) used to).
    ctx.bitget_features.values.erase("session_vwap");
    REQUIRE(dev().has_value());
    CHECK(std::isnan(*dev()));
}

TEST_CASE("crowding unwind needs every hard input measured") {
    StrategyEngine eng;
    auto ctx = make_ctx(100.0, "TREND_UP");
    ctx.funding_rate = 0.001;                                   // 10 bps / 8h
    ctx.cross_venue_features.values["funding_rate_8h"] = 0.001;
    ctx.cross_venue_features.values["funding_divergence"] = 0.0002;
    ctx.cross_venue_features.values["oi_growth_1h"] = 0.10;
    ctx.bitget_features.values["price_stall"] = 1.0;
    ctx.microstructure.values["cvd_delta"] = -2.0;              // flow weakening
    CHECK(fires(eng, ctx, "funding_oi_crowding_unwind"));

    // Boundary on the OI gate: just below 0.08 is not crowding.
    ctx.cross_venue_features.values["oi_growth_1h"] = 0.0799;
    CHECK_FALSE(fires(eng, ctx, "funding_oi_crowding_unwind"));
    ctx.cross_venue_features.values["oi_growth_1h"] = 0.10;

    // CVD absent: its 0 default satisfied `cvd <= 0`.
    ctx.microstructure.values.erase("cvd_delta");
    CHECK_FALSE(fires(eng, ctx, "funding_oi_crowding_unwind"));
    ctx.microstructure.values["cvd_delta"] = -2.0;

    // Funding absent: the consensus funding member must be there.
    ctx.cross_venue_features.values.erase("funding_rate_8h");
    CHECK_FALSE(fires(eng, ctx, "funding_oi_crowding_unwind"));
}

TEST_CASE("an absent trade imbalance never confirms a fade or an absorption") {
    StrategyEngine eng;
    // cvd_absorption_reversal at a fresh high with the tape selling into it.
    auto ctx = make_ctx(100.0, "TREND_UP");
    const double px = ctx.last_price();
    ctx.bitget_features.values["donchian_high_20"] = px;
    ctx.microstructure.values["cvd_delta"] = -1.0;
    ctx.microstructure.values["trade_imbalance"] = -0.2;
    ctx.market_forecast.present = false;
    CHECK(fires(eng, ctx, "cvd_absorption_reversal"));
    ctx.microstructure.values["trade_imbalance"] = 0.2;          // tape buying: no absorption
    CHECK_FALSE(fires(eng, ctx, "cvd_absorption_reversal"));
    ctx.microstructure.values.erase("trade_imbalance");          // unknown
    CHECK_FALSE(fires(eng, ctx, "cvd_absorption_reversal"));

    // range_extreme_mean_reversion at the top of the range.
    auto r = make_ctx(100.0, "RANGE");
    r.bitget_features.values["adx_14"] = 12.0;
    r.bitget_features.values["atr_expansion"] = 1.0;
    r.bitget_features.values["donchian_low_20"] = px * 0.98;
    r.bitget_features.values["donchian_high_20"] = px * 1.001;
    r.microstructure.values["trade_imbalance"] = -0.1;
    r.market_forecast.present = false;
    ProjectedZone z;
    z.kind = "resistance";
    z.center = px * 1.001;
    r.bitget_projected_zones.push_back(z);
    CHECK(fires(eng, r, "range_extreme_mean_reversion"));
    r.microstructure.values.erase("trade_imbalance");
    CHECK_FALSE(fires(eng, r, "range_extreme_mean_reversion"));
}

TEST_CASE("cross-venue agreement is read with its direction, so short setups are reachable") {
    // AltTech's agreement is a SHARE (>= 0.25 over 14 days); read as signed,
    // four strategies could only go long. The engine signs it with the
    // consensus direction -- and, for setups on the 15m grid, computes it on
    // the 15m grid (momentum_direction_agreement_15m) rather than reading the
    // 1m consensus minutes after the bar closed.
    StrategyEngine eng;

    // ema_compression_expansion: a compressed down-stack expanding, venues agreeing down.
    auto e = make_ctx(100.0, "TREND_DOWN");
    e.bitget_features.values["ema_20"] = 101.0;
    e.bitget_features.values["ema_50"] = 103.0;
    e.bitget_features.values["ema_compression"] = 0.002;
    e.bitget_features.values["atr_expansion"] = 1.2;
    // The venue agreement is a confirmation, recorded with its sign; the
    // short setup is reached from the EMA structure whatever it says.
    const auto agreement_of = [&](const DecisionContext& ctx, const char* id) -> std::optional<double> {
        for (const auto& c : eng.run(ctx, 1'000'000, {}))
            if (c.strategy_id == id) {
                CHECK(c.side == Side::short_);
                const auto it = c.descriptors.find("momentum_direction_agreement_15m");
                return it == c.descriptors.end() ? std::optional<double>(NAN) : std::optional<double>(it->second);
            }
        return std::nullopt;
    };
    e.bitget_features.values["momentum_direction_agreement_15m"] = -0.5;
    REQUIRE(agreement_of(e, "ema_compression_expansion") == -0.5);
    e.bitget_features.values["momentum_direction_agreement_15m"] = 0.5;    // venues pointing up
    REQUIRE(agreement_of(e, "ema_compression_expansion") == 0.5);          // recorded, not a veto
    e.bitget_features.values.erase("momentum_direction_agreement_15m");    // unknown: absent, never 0
    const auto unknown = agreement_of(e, "ema_compression_expansion");
    REQUIRE(unknown.has_value());
    CHECK(std::isnan(*unknown));

    // volume_weighted_momentum_burst: the burst is volume + flow with the move;
    // the venue agreement is recorded with its sign, both ways.
    auto v = make_ctx(100.0, "TREND_DOWN");
    v.bitget_features.values["mom_short"] = -0.004;
    v.microstructure.values["volume_surge"] = 2.5;
    v.microstructure.values["cvd_delta"] = -3.0;
    v.microstructure.values["trade_imbalance"] = -0.3;
    v.cross_venue_features.values["momentum_direction_agreement"] = -0.75;
    CHECK(fires(eng, v, "volume_weighted_momentum_burst"));
    v.cross_venue_features.values["momentum_direction_agreement"] = 0.75;
    CHECK(fires(eng, v, "volume_weighted_momentum_burst"));
    // Flow against the move is not a burst in that direction.
    v.microstructure.values["cvd_delta"] = 3.0;
    CHECK_FALSE(fires(eng, v, "volume_weighted_momentum_burst"));

    // donchian_breakout_continuation: a break DOWN out of the prior channel is
    // a short setup whatever a later minute's flow did.
    auto d = make_ctx(100.0, "BREAKOUT");
    d.bitget_features.values["donchian_high_20_prev"] = 105.0;
    d.bitget_features.values["donchian_low_20_prev"] = 100.5;
    d.bitget_features.values["atr_expansion"] = 1.2;
    d.cross_venue_features.values.erase("flow_direction_agreement");
    bool short_found = false;
    for (const auto& c : eng.run(d, 1'000'000, {}))
        if (c.strategy_id == "donchian_breakout_continuation") short_found = c.side == Side::short_;
    CHECK(short_found);

    // momentum_continuation follows Bitget's 1h regime, both ways.
    auto up = make_ctx(100.0, "TREND_UP");
    bool long_found = false;
    for (const auto& c : eng.run(up, 1'000'000, {}))
        if (c.strategy_id == "momentum_continuation") long_found = c.side == Side::long_;
    CHECK(long_found);
    auto dn = make_ctx(100.0, "TREND_DOWN");
    dn.bitget_features.values["ema_20"] = 101.0;
    dn.bitget_features.values["ema_50"] = 103.0;
    bool dn_found = false;
    for (const auto& c : eng.run(dn, 1'000'000, {}))
        if (c.strategy_id == "momentum_continuation") dn_found = c.side == Side::short_;
    CHECK(dn_found);
    // No 1h regime: no trend statement, no setup -- the 1m regime does not stand in.
    up.bitget_htf_regime.erase(60);
    CHECK_FALSE(fires(eng, up, "momentum_continuation"));
}

TEST_CASE("trend_pullback: Bitget 1h regime + a 15m pullback, both sides, no hand-set depth band") {
    StrategyEngine eng;
    // 1h up-trend, 15m EMAs stacked up, price pulled back under EMA20.
    auto c = make_ctx(100.0, "TREND_UP");
    c.bitget_features.values["ema_20"] = 101.0;
    c.bitget_features.values["ema_50"] = 99.0;
    // The 1m global regime says something else entirely: irrelevant now.
    c.global_regime.regime = "RANGE";
    c.bitget_regime.regime = "TREND_DOWN";
    bool long_found = false;
    for (const auto& x : eng.run(c, 1'000'000, {}))
        if (x.strategy_id == "trend_pullback") long_found = x.side == Side::long_;
    CHECK(long_found);
    // A shallow pullback (0.1 ATR) and a deep one (5 ATR) are both setups; the
    // depth is an input the model reads, not a gate.
    c.bitget_features.values["ema_20"] = 100.1;
    CHECK(fires(eng, c, "trend_pullback"));
    c.bitget_features.values["ema_20"] = 105.0;
    c.bitget_features.values["ema_50"] = 99.0;
    CHECK(fires(eng, c, "trend_pullback"));
    // Mirror: 1h down-trend, price bounced above a down-stacked EMA20.
    auto s = make_ctx(100.0, "TREND_DOWN");
    s.bitget_features.values["ema_20"] = 99.0;
    s.bitget_features.values["ema_50"] = 101.0;
    bool short_found = false;
    for (const auto& x : eng.run(s, 1'000'000, {}))
        if (x.strategy_id == "trend_pullback") short_found = x.side == Side::short_;
    CHECK(short_found);
    // Without the 1h regime there is no setup.
    s.bitget_htf_regime.erase(60);
    CHECK_FALSE(fires(eng, s, "trend_pullback"));
    // It is a CANDIDATE by default: never ACTIVE by construction.
    for (const auto& m : eng.metas())
        if (m.strategy_id == "trend_pullback") CHECK(m.default_lifecycle == Lifecycle::candidate);
}

TEST_CASE("unknown inputs no longer pass the dislocation, range and lead-lag guards") {
    StrategyEngine eng;
    // venue_dislocation_convergence: the consensus must be shown to be one level.
    auto x = make_ctx(100.0, "TREND_UP");
    x.cross_venue_features.values["bitget_mid_deviation_bps"] = 8.0;
    x.cross_venue_features.values["bitget_lead_score"] = 0.0;
    x.cross_venue_features.values["venue_price_dispersion_bps"] = 1.0;
    CHECK(fires(eng, x, "venue_dislocation_convergence"));
    x.cross_venue_features.values["venue_price_dispersion_bps"] = 10.0;   // venues disagree more than the gap
    CHECK_FALSE(fires(eng, x, "venue_dislocation_convergence"));
    x.cross_venue_features.values.erase("venue_price_dispersion_bps");    // unknown: used to skip the guard
    CHECK_FALSE(fires(eng, x, "venue_dislocation_convergence"));

    // cross_venue_lead_lag_follow: the flow is a confirmation now, recorded;
    // what must be measured is the laggard evidence itself.
    auto l = make_ctx(100.0, "TREND_UP");
    l.cross_venue_features.values["bitget_lead_score"] = -1.5;   // a full sample behind
    l.cross_venue_features.values["bitget_lag_ms"] = 500.0;
    l.cross_venue_features.values["bitget_mid_deviation_bps"] = -6.0;
    CHECK(fires(eng, l, "cross_venue_lead_lag_follow"));
    l.microstructure.values.erase("trade_imbalance");
    l.cross_venue_features.values.erase("consensus_flow");
    CHECK(fires(eng, l, "cross_venue_lead_lag_follow"));
    l.cross_venue_features.values.erase("bitget_lag_ms");    // laggard not shown by the lag
    CHECK_FALSE(fires(eng, l, "cross_venue_lead_lag_follow"));
}

TEST_CASE("lead-lag follows Bitget's own signed gap, both ways") {
    // It read the unsigned max venue deviation as Bitget's signed gap: longs
    // were unreachable and every venue's dislocation became a Bitget short.
    StrategyEngine eng;
    auto l = make_ctx(100.0, "TREND_UP");
    l.cross_venue_features.values["bitget_lead_score"] = -1.5;   // Bitget trails by > one sample
    l.cross_venue_features.values["bitget_lag_ms"] = 500.0;
    l.cross_venue_features.values["dislocation_bps"] = 9.0;      // unsigned magnitude, not read
    l.microstructure.values["trade_imbalance"] = 0.2;
    const auto side_of = [&]() -> std::optional<Side> {
        for (const auto& c : eng.run(l, 1'000'000, {}))
            if (c.strategy_id == "cross_venue_lead_lag_follow") return c.side;
        return std::nullopt;
    };
    l.cross_venue_features.values["bitget_mid_deviation_bps"] = -6.0;   // Bitget below the consensus
    CHECK(side_of() == Side::long_);
    l.microstructure.values["trade_imbalance"] = -0.2;
    l.cross_venue_features.values["bitget_mid_deviation_bps"] = 6.0;    // above
    CHECK(side_of() == Side::short_);
    // The gap must exceed quote noise: half the observed spread (it was a
    // hand-set 4 bps). A measured 8 bps spread puts the bound at 4 bps.
    l.spread_observed = true;
    l.spread_bps = 8.0;
    l.cross_venue_features.values["bitget_mid_deviation_bps"] = 3.9;
    CHECK_FALSE(side_of().has_value());
    l.cross_venue_features.values["bitget_mid_deviation_bps"] = 4.1;
    CHECK(side_of() == Side::short_);
    // Flow against the catch-up no longer vetoes it; it is recorded.
    l.microstructure.values["trade_imbalance"] = 0.5;
    CHECK(side_of() == Side::short_);
    // "Trails" is measured in samples of AltTech's 1m grid: less than one full
    // sample is below the measurement's resolution -- Bitget's score is
    // negative in 98 % of contexts, so a sign alone is a state, not an event.
    l.cross_venue_features.values["bitget_lead_score"] = -0.5;
    CHECK_FALSE(side_of().has_value());
    l.cross_venue_features.values["bitget_lead_score"] = -1.0;
    CHECK(side_of() == Side::short_);
    // Standing off the consensus means a two-dispersion outlier: with the
    // venues 3 bps apart among themselves, a 4.1 bps gap is not one.
    l.cross_venue_features.values["venue_price_dispersion_bps"] = 3.0;
    CHECK_FALSE(side_of().has_value());
    l.cross_venue_features.values["bitget_mid_deviation_bps"] = 6.1;
    CHECK(side_of() == Side::short_);
    l.cross_venue_features.values["venue_price_dispersion_bps"] = 1.0;
    l.cross_venue_features.values["bitget_mid_deviation_bps"] = 4.1;
    l.cross_venue_features.values.erase("bitget_mid_deviation_bps");    // unknown
    CHECK_FALSE(side_of().has_value());
}

// ---------------------------------------------------------------------------
// Two-sidedness, for all sixteen. A setup detector must not be one-sided
// because of how a feature is read (the signed/unsigned confusion made four
// strategies long-only). The test mirrors a context through the price: every
// level is reflected about it, every signed quantity changes sign, TREND_UP
// and TREND_DOWN swap, support and resistance swap. A setup that fires on a
// context must fire on the OTHER side on its mirror image.
// ---------------------------------------------------------------------------
namespace {

DecisionContext mirror(const DecisionContext& c) {
    DecisionContext m = c;
    const double px = c.last_price();
    const auto flip = [&](FeatureSummary& f, const char* k) {
        if (f.has(k)) f.values[k] = 2 * px - f.get(k);
    };
    const auto neg = [](FeatureSummary& f, const char* k) {
        if (f.has(k)) f.values[k] = -f.get(k);
    };
    const auto swap_hilo = [&](FeatureSummary& f, const char* hi, const char* lo) {
        if (f.has(hi) && f.has(lo)) {
            const double h = f.get(hi), l = f.get(lo);
            f.values[hi] = 2 * px - l;
            f.values[lo] = 2 * px - h;
        }
    };
    const auto price_block = [&](FeatureSummary& f) {
        for (const char* k : {"ema_20", "ema_50", "ema_200", "vwap", "session_vwap", "bb_middle_20", "open"})
            flip(f, k);
        swap_hilo(f, "high", "low");
        swap_hilo(f, "donchian_high_20", "donchian_low_20");
        swap_hilo(f, "donchian_high_20_prev", "donchian_low_20_prev");
        swap_hilo(f, "bb_upper_20", "bb_lower_20");
        for (const char* k : {"mom_1m", "mom_5m", "mom_15m", "mom_short", "mom_5", "mom_14", "roc_5",
                              "roc_14", "roc_20", "vwap_dev", "session_vwap_dev_bps",
                              "momentum_direction_agreement_15m"})
            neg(f, k);
        if (f.has("rsi_14")) f.values["rsi_14"] = 100.0 - f.get("rsi_14");
    };
    price_block(m.bitget_features);
    for (auto& [tf, f] : m.context_features) price_block(f);
    for (const char* k : {"momentum_direction_agreement", "flow_direction_agreement",
                          "bitget_mid_deviation_bps", "funding_rate_8h", "funding_divergence",
                          "consensus_flow", "consensus_momentum_score"})
        neg(m.cross_venue_features, k);
    if (m.cross_venue_features.has("consensus_mark_price") && m.index_price > 0)
        m.cross_venue_features.values["consensus_mark_price"] =
            2 * m.index_price - m.cross_venue_features.get("consensus_mark_price");
    for (const char* k : {"trade_imbalance", "cvd_delta", "book_imbalance", "book_microprice_dev_bps"})
        neg(m.microstructure, k);
    m.funding_rate = -m.funding_rate;
    const auto swap_regime = [](std::string& r) {
        if (r == "TREND_UP") r = "TREND_DOWN";
        else if (r == "TREND_DOWN") r = "TREND_UP";
    };
    swap_regime(m.bitget_regime.regime);
    swap_regime(m.global_regime.regime);
    for (auto& [tf, r] : m.bitget_htf_regime) swap_regime(r.regime);
    for (auto& z : m.bitget_projected_zones) {
        if (z.kind == "support") z.kind = "resistance";
        else if (z.kind == "resistance") z.kind = "support";
        const double lo = z.lower, hi = z.upper;
        z.center = 2 * px - z.center;
        z.lower = 2 * px - hi;
        z.upper = 2 * px - lo;
    }
    for (auto& [h, f] : m.forecasts) {
        std::swap(f.p_long, f.p_short);
        std::swap(f.expected_net_long_bps, f.expected_net_short_bps);
    }
    return m;
}

std::optional<Side> side_of(const StrategyEngine& eng, const DecisionContext& ctx, const std::string& id) {
    for (const auto& c : eng.run(ctx, 1'000'050, {}))
        if (c.strategy_id == id) return c.side;
    return std::nullopt;
}

} // namespace

TEST_CASE("every one of the sixteen setups fires on both sides (mirror symmetry)", "[strategies][symmetry]") {
    StrategyEngine eng;
    std::map<std::string, DecisionContext> fx;
    const double px = 100.0;
    {   auto c = make_ctx(px, "TREND_UP"); fx["momentum_continuation"] = c; }
    {   auto c = make_ctx(px, "TREND_UP");
        c.bitget_features.values["ema_20"] = 101.0; c.bitget_features.values["ema_50"] = 99.0;
        fx["trend_pullback"] = c; }
    {   auto c = make_ctx(px, "TREND_UP");
        c.bitget_features.values["mom_short"] = 0.004;
        FeatureSummary g; g.valid = true; g.values = {{"roc_14", 0.002}};
        c.context_features[30] = g; c.context_features[60] = g;
        c.bitget_features.values["session_vwap"] = px * 0.99;
        fx["multi_timeframe_momentum_alignment"] = c; }
    {   auto c = make_ctx(px, "TREND_UP");
        c.bitget_features.values["mom_short"] = 0.004;
        c.microstructure.values["volume_surge"] = 2.5; c.microstructure.values["cvd_delta"] = 3.0;
        c.microstructure.values["trade_imbalance"] = 0.3;
        c.cross_venue_features.values["momentum_direction_agreement"] = 0.75;
        fx["volume_weighted_momentum_burst"] = c; }
    {   auto c = make_ctx(px, "BREAKOUT");
        c.bitget_features.values["donchian_high_20_prev"] = px * 0.995;
        fx["donchian_breakout_continuation"] = c; }
    {   auto c = make_ctx(px, "TREND_UP");
        c.bitget_features.values["ema_compression"] = 0.002; c.bitget_features.values["atr_expansion"] = 1.2;
        fx["ema_compression_expansion"] = c; }
    {   auto c = make_ctx(px, "TREND_UP");
        c.index_price = 100.0;
        c.cross_venue_features.values["consensus_mark_price"] = 100.1;   // +10 bps basis
        c.cross_venue_features.values["funding_rate_8h"] = 0.001;
        fx["funding_basis_convergence"] = c; }
    {   auto c = make_ctx(px, "TREND_UP");
        c.cross_venue_features.values["bitget_mid_deviation_bps"] = 8.0;
        c.cross_venue_features.values["bitget_lead_score"] = 0.0;
        c.cross_venue_features.values["venue_price_dispersion_bps"] = 1.0;
        fx["venue_dislocation_convergence"] = c; }
    {   auto c = make_ctx(px, "RANGE");
        FeatureSummary h; h.valid = true;
        h.values = {{"adx_14", 15.0}, {"donchian_high_20", 101.0}, {"donchian_low_20", 97.0}};
        c.context_features[240] = h;
        fx["htf_range_false_breakout_fade"] = c; }
    {   auto c = make_ctx(px, "TREND_UP");
        c.microstructure.values["book_imbalance"] = 0.4;
        c.microstructure.values["book_microprice_dev_bps"] = 1.0;
        c.spread_observed = true; c.depth_observed = true;   // a book trade needs a measured book
        fx["book_imbalance_pressure"] = c; }
    {   auto c = make_ctx(px, "TREND_UP");
        c.bitget_features.values["donchian_high_20"] = px;
        c.microstructure.values["cvd_delta"] = -1.0; c.microstructure.values["trade_imbalance"] = -0.2;
        fx["cvd_absorption_reversal"] = c; }
    {   auto c = make_ctx(px, "TREND_UP");
        c.spread_bps = 10.0; c.spread_observed = true;
        c.top_depth_usdt = 50'000; c.depth_observed = true; c.liquidity_rank = 1.0;
        c.microstructure.values["book_microprice_dev_bps"] = 2.0;
        c.microstructure.values["spread_bps"] = 10.0;              // the measured book members
        c.microstructure.values["book_bid_depth"] = 25'000;
        c.microstructure.values["book_ask_depth"] = 25'000;
        fx["spread_dislocation_fade"] = c; }
    {   auto c = make_ctx(px, "RANGE");
        c.bitget_features.values["adx_14"] = 12.0; c.bitget_features.values["atr_expansion"] = 1.0;
        c.bitget_features.values["donchian_low_20"] = px * 0.98;
        c.bitget_features.values["donchian_high_20"] = px * 1.001;
        c.microstructure.values["trade_imbalance"] = -0.1;
        ProjectedZone z; z.kind = "resistance"; z.center = px * 1.001; z.lower = px; z.upper = px * 1.002;
        c.bitget_projected_zones.push_back(z);
        fx["range_extreme_mean_reversion"] = c; }
    {   auto c = make_ctx(px, "TREND_UP");
        c.funding_rate = 0.001;
        c.cross_venue_features.values["funding_rate_8h"] = 0.001;
        c.cross_venue_features.values["funding_divergence"] = 0.0002;
        c.cross_venue_features.values["oi_growth_1h"] = 0.10;
        c.bitget_features.values["price_stall"] = 1.0;
        c.microstructure.values["cvd_delta"] = -2.0;
        fx["funding_oi_crowding_unwind"] = c; }
    {   auto c = make_ctx(px, "RANGE");
        c.bitget_features.values["session_vwap"] = 99.9;
        c.bitget_features.values["session_vwap_dev_bps"] = 10.0;
        c.bitget_features.values["session_vwap_lost_recently"] = 1.0;
        c.microstructure.values["cvd_delta"] = 3.0;
        fx["vwap_reclaim_continuation"] = c; }
    {   auto c = make_ctx(px, "TREND_UP");
        c.cross_venue_features.values["bitget_lead_score"] = -1.5;
        c.cross_venue_features.values["bitget_lag_ms"] = 500.0;
        c.cross_venue_features.values["bitget_mid_deviation_bps"] = -6.0;
        fx["cross_venue_lead_lag_follow"] = c; }

    REQUIRE(fx.size() == 16);
    REQUIRE(eng.size() == 16);
    for (const auto& [id, ctx] : fx) {
        INFO("strategy: " << id);
        const auto a = side_of(eng, ctx, id);
        REQUIRE(a.has_value());
        const auto b = side_of(eng, mirror(ctx), id);
        REQUIRE(b.has_value());
        CHECK(*b == opposite(*a));
        // Every setup describes itself in measured terms, and carries the
        // market view on all three horizons.
        for (const auto& c : eng.run(ctx, 1'000'000, {})) {
            if (c.strategy_id != id) continue;
            CHECK_FALSE(c.descriptors.empty());
            for (const auto& [name, v] : c.descriptors) CHECK(std::isfinite(v));
            REQUIRE(c.predictions.size() == 3);
            CHECK(c.predictions[0].horizon_minutes == 15);
            CHECK(c.predictions[1].horizon_minutes == 60);
            CHECK(c.predictions[2].horizon_minutes == 240);
        }
    }
}

TEST_CASE("one open hypothesis per pair, setup and side: a persisting state publishes once per horizon",
          "[strategies][vps1][regression]") {
    HypothesisBook book;
    const int64_t h = 4LL * 3600 * 1000;
    const auto admit_at = [&](int64_t pair, const char* id, Side side, int64_t t) {
        return book.admit(pair, id, side, t, h, 100.0, 1.0).admitted;
    };
    CHECK(admit_at(4, "momentum_continuation", Side::long_, 1'000'000));
    // The same trend on the next decision bars: the same hypothesis, still open.
    CHECK_FALSE(admit_at(4, "momentum_continuation", Side::long_, 1'000'000 + 15 * 60'000));
    CHECK_FALSE(admit_at(4, "momentum_continuation", Side::long_, 1'000'000 + h - 1));
    // Other pairs, other setups and the other side are other hypotheses.
    CHECK(admit_at(5, "momentum_continuation", Side::long_, 1'000'000 + 60'000));
    CHECK(admit_at(4, "trend_pullback", Side::long_, 1'000'000 + 60'000));
    CHECK(admit_at(4, "momentum_continuation", Side::short_, 1'000'000 + 60'000));
    // Once its horizon has elapsed the state is a new statement.
    CHECK(admit_at(4, "momentum_continuation", Side::long_, 1'000'000 + h));

    // Through the engine: the repeat is counted, not silently lost.
    StrategyEngine eng;
    auto ctx = make_ctx(100.0, "TREND_UP");
    HypothesisBook b2;
    const auto admit = [&](const std::string& id, Side side, int64_t horizon_ms) {
        return b2.admit(ctx.canonical_pair_id, id, side, ctx.event_time_ms, horizon_ms, ctx.last_price(), ctx.atr())
            .admitted;
    };
    StrategyFunnel f1, f2;
    const auto first = eng.run(ctx, 1'000'100, {}, &f1, admit);
    ctx.event_time_ms += 15 * 60'000;
    const auto again = eng.run(ctx, 1'000'100 + 15 * 60'000, {}, &f2, admit);
    bool mc_first = false, mc_again = false;
    for (const auto& c : first) mc_first |= c.strategy_id == "momentum_continuation";
    for (const auto& c : again) mc_again |= c.strategy_id == "momentum_continuation";
    CHECK(mc_first);
    CHECK_FALSE(mc_again);
    CHECK(f2.by_strategy.at("momentum_continuation").repeat_within_horizon == 1);
    const auto& c2 = f2.by_strategy.at("momentum_continuation");
    int64_t inel = 0;
    for (const auto& [r, n] : c2.ineligible) inel += n;
    CHECK(inel + c2.no_setup + c2.geometry_rejected + c2.horizon_refused + c2.repeat_within_horizon +
              c2.candidates == c2.contexts);
}

TEST_CASE("an open hypothesis survives a restart, and the reset before a repeat is measured, not acted on",
          "[strategies][vps1][design-04]") {
    const int64_t h = 60LL * 60 * 1000;
    // Restored from the archive: a setup inside its horizon is still a repeat.
    HypothesisBook book;
    book.restore(4, "vwap_reclaim_continuation", Side::long_, 1'000'000 + h, 100.0, 2.0);
    CHECK_FALSE(book.admit(4, "vwap_reclaim_continuation", Side::long_, 1'000'000 + 60'000, h, 100.0, 2.0).admitted);
    CHECK(book.admit(4, "vwap_reclaim_continuation", Side::long_, 1'000'000 + h, h, 100.0, 2.0).admitted);
    // An older restore never shortens what is already known to be open.
    book.restore(4, "vwap_reclaim_continuation", Side::long_, 1'000'000, 100.0, 2.0);
    CHECK_FALSE(book.admit(4, "vwap_reclaim_continuation", Side::long_, 1'000'000 + h + 60'000, h, 100.0, 2.0).admitted);

    // Reset depth: how far price went AGAINST the hypothesis after it opened.
    HypothesisBook b;
    CHECK(b.admit(7, "donchian_breakout_continuation", Side::long_, 0, h, 100.0, 2.0).reset_atr < 0);
    b.observe(7, 101.0);   // with the long: no reset
    b.observe(7, 97.0);    // 3.0 against it = 1.5 ATR
    b.observe(7, 99.0);
    const auto again = b.admit(7, "donchian_breakout_continuation", Side::long_, 60'000, h, 99.0, 2.0);
    CHECK_FALSE(again.admitted);   // the book still says repeat: measurement changes nothing
    CHECK(again.reset_atr == Catch::Approx(1.5));
    CHECK(reset_bucket(again.reset_atr) == 2);
    // A short is reset by a rise.
    CHECK(b.admit(7, "donchian_breakout_continuation", Side::short_, 0, h, 100.0, 2.0).admitted);
    b.observe(7, 100.5);
    CHECK(b.admit(7, "donchian_breakout_continuation", Side::short_, 60'000, h, 100.0, 2.0).reset_atr ==
          Catch::Approx(0.25));
    CHECK(reset_bucket(0.0) == 0);
    CHECK(reset_bucket(0.5) == 1);
    CHECK(reset_bucket(2.0) == 3);
}

TEST_CASE("a lopsided book means one side holds at least twice the other's depth",
          "[strategies][vps1]") {
    StrategyEngine eng;
    auto ctx = make_ctx(100.0, "TREND_UP");
    ctx.spread_observed = true;
    ctx.depth_observed = true;
    ctx.microstructure.values["book_microprice_dev_bps"] = 0.3;
    ctx.microstructure.values["book_imbalance"] = 0.30;          // 13:7 -- not twice
    CHECK_FALSE(fires(eng, ctx, "book_imbalance_pressure"));
    ctx.microstructure.values["book_imbalance"] = 0.34;          // just over 2:1
    CHECK(fires(eng, ctx, "book_imbalance_pressure"));
    ctx.spread_bps = 12.0;                                       // a wide book: recorded, not refused
    CHECK(fires(eng, ctx, "book_imbalance_pressure"));
}
