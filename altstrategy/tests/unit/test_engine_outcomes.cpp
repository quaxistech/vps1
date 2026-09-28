// Market outcome reconstruction, ambiguity, counterfactuals, drift, calibration
// and lifecycle tests (§18, §19, §20, §35, §36, §40, §41).

#include "altstrategy/engine/adapt.hpp"
#include "altstrategy/engine/attribution.hpp"
#include "altstrategy/engine/outcomes.hpp"
#include "engine_test_helpers.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <map>

using namespace altstrategy::engine;
using namespace altstrategy::engine::test;

static OutcomeGeometry long_geo() {
    OutcomeGeometry g;
    g.side = Side::long_;
    g.entry_type = EntryType::market;
    g.entry_price = 100.0;
    g.stop_loss_price = 99.0;
    g.take_profit_price = 102.0;
    g.created_at_ms = 0;
    g.entry_fill_timeout_ms = 5LL * 60 * 1000;
    g.valid_until_ms = 30LL * 60 * 1000;
    g.time_stop_at_ms = 60LL * 60 * 1000;
    g.cost_bps = 8.0;
    return g;
}

TEST_CASE("TARGET_FIRST when the path reaches the target") {
    const auto g = long_geo();
    auto bars = up_path(0, 100.0, 103.0, 30);   // rises through 102
    const auto o = reconstruct_outcome(g, bars, OutcomePath::m1_candles, "c1", "s1", 100);
    CHECK(o.outcome == MarketOutcomeType::target_first);
    CHECK(o.realized_net_return > 0);
    CHECK(o.mfe > 0);
    CHECK(o.entry_time_ms == 0);   // market entry immediate
}

TEST_CASE("a fill already past the target or stop is ENTRY_EXPIRED, not a trade") {
    // A MARKET entry fills at the bar open and the SAME bar is scanned for
    // target/stop. On a gap through the target the bar trivially touches it, so
    // the trade was recorded TARGET_FIRST while exiting AT the target was worse
    // than the fill -- a label contradicting its own economics. Measured on
    // VPS1: 2 of 4309 TARGET_FIRST had a negative gross return, 1 of 7067
    // STOP_FIRST a positive one. Every statistic built on those labels -- win
    // rate, expectancy, the posterior that ranks the strategy -- inherits it.
    const auto g = long_geo();   // entry 100, stop 99, target 102

    SECTION("gapped through the target") {
        // Opens at 103: already past the 102 target before any decision.
        auto bars = up_path(0, 103.0, 105.0, 10);
        const auto o = reconstruct_outcome(g, bars, OutcomePath::m1_candles, "c1", "s1", 100);
        CHECK(o.outcome == MarketOutcomeType::entry_expired);
        CHECK(o.realized_gross_return == Catch::Approx(0.0));
    }

    SECTION("gapped through the stop") {
        // Opens at 98: already past the 99 stop.
        auto bars = up_path(0, 98.0, 98.5, 10);
        const auto o = reconstruct_outcome(g, bars, OutcomePath::m1_candles, "c2", "s1", 100);
        CHECK(o.outcome == MarketOutcomeType::entry_expired);
    }

    SECTION("a normal fill inside the plan still resolves") {
        auto bars = up_path(0, 100.0, 103.0, 30);
        const auto o = reconstruct_outcome(g, bars, OutcomePath::m1_candles, "c3", "s1", 100);
        CHECK(o.outcome == MarketOutcomeType::target_first);
        CHECK(o.realized_gross_return > 0);   // label and economics agree
    }
}

TEST_CASE("STOP_FIRST when the path falls to the stop") {
    const auto g = long_geo();
    std::vector<PathBar> bars;
    for (int i = 0; i < 10; ++i) {
        PathBar b; b.open_ms = i * 60'000;
        const double mid = 100.0 - i * 0.3;
        b.open = mid; b.high = mid + 0.05; b.low = mid - 0.2; b.close = mid;
        bars.push_back(b);
    }
    const auto o = reconstruct_outcome(g, bars, OutcomePath::m1_candles, "c1", "s1", 100);
    CHECK(o.outcome == MarketOutcomeType::stop_first);
    CHECK(o.realized_net_return < 0);
}

TEST_CASE("AMBIGUOUS when stop and target fall in one 1m bar (§19)") {
    const auto g = long_geo();
    std::vector<PathBar> bars;
    PathBar b; b.open_ms = 0; b.open = 100.0; b.high = 102.5; b.low = 98.5; b.close = 100.0;
    bars.push_back(b);
    const auto o = reconstruct_outcome(g, bars, OutcomePath::m1_candles, "c1", "s1", 100);
    CHECK(o.outcome == MarketOutcomeType::ambiguous);
    CHECK(o.ambiguous);
    // Ambiguous is never scored as a win or loss.
    CHECK(o.realized_net_return == 0.0);
}

TEST_CASE("ENTRY_NOT_REACHED when a limit entry is never touched (price runs away)") {
    auto g = long_geo();
    g.entry_type = EntryType::limit;
    g.entry_price = 98.0;   // buy limit below; price only goes up and hits target
    std::vector<PathBar> bars = up_path(0, 100.0, 103.0, 10);
    const auto o = reconstruct_outcome(g, bars, OutcomePath::m1_candles, "c1", "s1", 100);
    CHECK(o.outcome == MarketOutcomeType::entry_not_reached);
}

TEST_CASE("TIME_STOP when neither level is hit before the time stop") {
    auto g = long_geo();
    g.time_stop_at_ms = 10LL * 60 * 1000;
    std::vector<PathBar> bars;
    for (int i = 0; i <= 12; ++i) {   // drift sideways, cover past the time-stop
        PathBar b; b.open_ms = i * 60'000; b.open = 100.0; b.high = 100.3; b.low = 99.7; b.close = 100.1;
        bars.push_back(b);
    }
    const auto o = reconstruct_outcome(g, bars, OutcomePath::m1_candles, "c1", "s1", 100);
    CHECK(o.outcome == MarketOutcomeType::time_stop);
}

TEST_CASE("finer (raw-trade) path resolves a same-window stop+target instead of AMBIGUOUS") {
    const auto g = long_geo();
    std::vector<PathBar> bars;
    PathBar b; b.open_ms = 0; b.open = 100.0; b.high = 102.5; b.low = 98.5; b.close = 100.0;
    bars.push_back(b);
    const auto o = reconstruct_outcome(g, bars, OutcomePath::raw_trades, "c1", "s1", 100);
    CHECK(o.outcome != MarketOutcomeType::ambiguous);
}

TEST_CASE("counterfactual regret: a better non-selected candidate raises regret") {
    MarketOutcome sel; sel.candidate_id = "sel"; sel.outcome = MarketOutcomeType::stop_first;
    sel.realized_net_return = -0.01;
    MarketOutcome cf; cf.candidate_id = "cf"; cf.outcome = MarketOutcomeType::target_first;
    cf.realized_net_return = 0.02;
    const auto out = compare_counterfactuals("rank-1", sel, {cf}, 100);
    REQUIRE(out.size() == 1);
    CHECK(out[0].best_hindsight);
    CHECK(out[0].ranking_regret > 0.0);   // hindsight best beat the pick
}

TEST_CASE("drift status escalates with outcome degradation") {
    DriftInputs in;
    in.sample_count = 200;
    in.baseline_sample_count = 100; in.recent_sample_count = 100;
    in.baseline_win_rate = 0.55; in.recent_win_rate = 0.20;   // large outcome drift
    in.baseline_score_mean = 0.5; in.recent_score_mean = 0.48;
    in.baseline_regime_perf = 0.01; in.recent_regime_perf = -0.02;
    const auto d = compute_drift("s", "1", in, 100);
    CHECK((d.status == DriftStatus::degraded || d.status == DriftStatus::structural_break));
    CHECK(d.outcome_drift > 0.35);
}

TEST_CASE("calibration stays within schema bounds and needs samples") {
    std::vector<StrategyParam> schema = {{"stop_atr", "double", 1.0, 4.0, 0.1, 2.0},
                                         {"target_atr", "double", 1.5, 8.0, 0.1, 3.0}};
    std::map<std::string, double> cur = {{"stop_atr", 2.0}, {"target_atr", 3.0}};
    CalibrationTargets t; t.sample_count = 10;   // too few
    CHECK(calibrate_parameters(schema, cur, t) == cur);
    t.sample_count = 100;
    t.predicted_mae = 40; t.actual_mae = 60;     // realized adverse excursion larger → widen stop
    t.predicted_mfe = 90; t.actual_mfe = 70;
    const auto out = calibrate_parameters(schema, cur, t);
    CHECK(out.at("stop_atr") >= 1.0);
    CHECK(out.at("stop_atr") <= 4.0);
    CHECK(out.at("stop_atr") > cur.at("stop_atr"));   // widened
}

TEST_CASE("lifecycle: ACTIVE demotes on structural break; promotion needs samples") {
    LifecycleInputs in;
    in.current = Lifecycle::active; in.drift = DriftStatus::structural_break;
    in.sample_count = 500; in.cooldown_elapsed = true;
    const auto t1 = evaluate_lifecycle("s", "1", in, 100);
    REQUIRE(t1.has_value());
    CHECK(t1->to_status == Lifecycle::degraded);

    LifecycleInputs promo;
    promo.current = Lifecycle::shadow; promo.drift = DriftStatus::stable;
    promo.posterior_edge = 0.01; promo.posterior_uncertainty = 0.005;
    promo.sample_count = 50; promo.min_samples = 100; promo.cooldown_elapsed = true;
    CHECK_FALSE(evaluate_lifecycle("s", "1", promo, 100).has_value());   // too few samples
    promo.sample_count = 150;
    const auto t2 = evaluate_lifecycle("s", "1", promo, 100);
    REQUIRE(t2.has_value());
    CHECK(t2->to_status == Lifecycle::candidate);

    // Cooldown blocks any transition.
    LifecycleInputs cd = promo; cd.cooldown_elapsed = false;
    CHECK_FALSE(evaluate_lifecycle("s", "1", cd, 100).has_value());
}

// Regression: a stalled chain used to be read as a collapse in win rate.
// The guard counted baseline + recent together, so a rich baseline carried an
// empty recent window past it, `recent_win_rate` defaulted to 0.0, and the
// resulting STRUCTURAL_BREAK held the strategies DEGRADED — which stopped them
// producing the outcomes that would have cleared the verdict.
TEST_CASE("drift needs evidence in both windows, not just in total",
          "[drift][regression]") {
    SECTION("an empty recent window is not a collapse") {
        // The exact production shape: momentum_continuation on 2026-08-24.
        DriftInputs in;
        in.baseline_sample_count = 3339;
        in.recent_sample_count = 0;
        in.sample_count = 3339;              // passes the old combined guard
        in.baseline_win_rate = 0.3291;
        in.recent_win_rate = 0.0;            // no trades, not lost trades

        const auto d = compute_drift("momentum_continuation", "1.0.0", in, 100);
        CHECK(d.status == DriftStatus::stable);
        CHECK(d.status != DriftStatus::structural_break);
    }

    SECTION("three recent trades cannot condemn a strategy") {
        // multi_timeframe_momentum_alignment, same day: 3 recent outcomes, all
        // winners, against a baseline of 1653.
        DriftInputs in;
        in.baseline_sample_count = 1653;
        in.recent_sample_count = 3;
        in.sample_count = 1656;
        in.baseline_win_rate = 0.4301;
        in.recent_win_rate = 1.0;

        const auto d = compute_drift("multi_timeframe_momentum_alignment", "1.0.0", in, 100);
        CHECK(d.status == DriftStatus::stable);
    }

    SECTION("a thin baseline is refused the same way") {
        DriftInputs in;
        in.baseline_sample_count = 5;
        in.recent_sample_count = 500;
        in.sample_count = 505;
        in.baseline_win_rate = 0.9;
        in.recent_win_rate = 0.1;
        CHECK(compute_drift("s", "1", in, 100).status == DriftStatus::stable);
    }

    SECTION("a real collapse, evidenced on both sides, still escalates") {
        DriftInputs in;
        in.baseline_sample_count = 500;
        in.recent_sample_count = 500;
        in.sample_count = 1000;
        in.baseline_win_rate = 0.55;
        in.recent_win_rate = 0.05;
        in.baseline_regime_perf = 0.01;
        in.recent_regime_perf = -0.05;

        const auto d = compute_drift("s", "1", in, 100);
        CHECK(d.status != DriftStatus::stable);
        CHECK(d.outcome_drift > 0.35);
    }
}

// Regression: drift in a mean return was measured by `rel()`, which is
// scale-free and therefore returns exactly 1.0 for ANY sign flip however small.
// momentum_continuation scored regime_performance_drift = 0.977 —
// STRUCTURAL_BREAK — on mean returns a couple of basis points either side of
// zero, and STRUCTURAL_BREAK blocks the recovery path out of DEGRADED forever.
TEST_CASE("drift in a mean is judged against its uncertainty, not its magnitude",
          "[drift][regression]") {
    // Helper: build inputs whose only live signal is regime performance.
    const auto regime_drift = [](double bm, double bsd, int64_t bn,
                                 double rm, double rsd, int64_t rn) {
        DriftInputs in;
        in.baseline_sample_count = 500;
        in.recent_sample_count = 500;
        in.sample_count = 1000;
        in.baseline_win_rate = 0.40; in.recent_win_rate = 0.40;
        in.baseline_regime_perf = bm; in.baseline_regime_perf_sd = bsd;
        in.baseline_regime_groups = bn;
        in.recent_regime_perf = rm;   in.recent_regime_perf_sd = rsd;
        in.recent_regime_groups = rn;
        return compute_drift("s", "1", in, 100).regime_performance_drift;
    };

    SECTION("a sign flip inside the noise is not maximal drift") {
        // -1 bp -> +1 bp, with the group means scattered over ~50 bps.
        const double d = regime_drift(-0.0001, 0.005, 6, 0.0001, 0.005, 6);
        CHECK(d < 0.20);          // nowhere near the 0.35 degraded band
        CHECK(d < 0.977);         // and far below what rel() scored it
    }

    SECTION("a large, well-evidenced deterioration does register") {
        // +100 bps -> -100 bps on tight groups: a real regime-level collapse.
        const double d = regime_drift(0.010, 0.002, 8, -0.010, 0.002, 8);
        CHECK(d > 0.6);           // structural break, correctly
    }

    SECTION("magnitude alone no longer outranks significance") {
        const double noisy_flip = regime_drift(-0.0001, 0.005, 6, 0.0001, 0.005, 6);
        const double real_drop  = regime_drift(0.010, 0.002, 8, 0.004, 0.002, 8);
        // The economically meaningful change must score higher than the
        // meaningless one. Under rel() the ordering was inverted: 1.0 vs 0.43.
        CHECK(real_drop > noisy_flip);
    }

    SECTION("too few groups yields no verdict rather than a fabricated one") {
        CHECK(regime_drift(-0.0001, 0.0, 1, 0.0001, 0.0, 1) == 0.0);
        CHECK(regime_drift(-0.0001, 0.0, 0, 0.0001, 0.0, 0) == 0.0);
    }

    SECTION("more evidence for the same effect means more drift") {
        const double thin  = regime_drift(0.002, 0.004, 3, -0.002, 0.004, 3);
        const double thick = regime_drift(0.002, 0.004, 30, -0.002, 0.004, 30);
        CHECK(thick > thin);      // same effect size, stronger evidence
    }
}

TEST_CASE("attribution never credits or blames a model that produced nothing") {
    // ml_attribution read proposal.expected_net_return unconditionally. With no
    // active model that field held -cost (a negative number on every trade), so
    // the ML term was charged for losses the model had not predicted and could
    // not have. It is now gated on edge_estimated.
    MarketOutcome m;
    m.realized_gross_return = -0.004;   // the trade lost
    m.realized_net_return = -0.006;
    m.mfe = 0.001;
    m.mae = 0.005;

    TradeSignalProposal no_model;
    no_model.signal_proposal_id = "p-no-model";
    no_model.edge_estimated = false;
    no_model.expected_net_return = -0.0016;   // what -cost used to look like
    no_model.round_trip_cost_bps = 16.0;
    const auto a1 = attribute_outcome(no_model, m, std::nullopt, 0.0, false, 1'000'000);
    CHECK(a1.ml_attribution == 0.0);
    // Cost comes from the versioned figure, not from a gross-minus-net gap.
    CHECK(a1.cost_attribution == Catch::Approx(-0.0016));

    // A real forecast, pointing the right way, IS attributed.
    TradeSignalProposal with_model = no_model;
    with_model.signal_proposal_id = "p-model";
    with_model.edge_estimated = true;
    with_model.expected_net_return = -0.003;   // model predicted a loss; it happened
    const auto a2 = attribute_outcome(with_model, m, std::nullopt, 0.0, false, 1'000'000);
    CHECK(a2.ml_attribution != 0.0);
}

// Regression: the lifecycle pass and the decision path disagreed about what
// state a strategy was in.
//
// `strategy_engine.cpp` resolves a candidate's lifecycle as `override ?? the
// status the strategy ships as`. The maintenance pass resolved the same fact as
// `override ?? ACTIVE`. Only 2 of the 16 shipped strategies declare ACTIVE, so
// for the other 14 the two paths disagreed: graded SHADOW/EXPERIMENTAL and
// never executable on one side, treated as ACTIVE on the other -- and
// `evaluate_lifecycle`'s ACTIVE branch only demotes. The promotion ladder was
// therefore unreachable for every strategy that starts below ACTIVE, and 5 of
// the 6 `ACTIVE -> DEGRADED` rows in lifecycle_events name a status those
// strategies never held.
TEST_CASE("the lifecycle pass reads the status a strategy actually ships as",
          "[lifecycle][regression]") {
    const std::map<std::string, Lifecycle> declared{
        {"momentum_continuation", Lifecycle::active},
        {"multi_timeframe_momentum_alignment", Lifecycle::candidate},
        {"donchian_breakout_continuation", Lifecycle::shadow},
        {"venue_dislocation_convergence", Lifecycle::research}};

    SECTION("no override means the declared status, not ACTIVE") {
        const std::map<std::string, Lifecycle> no_overrides;
        CHECK(current_lifecycle("donchian_breakout_continuation", no_overrides, declared) ==
              Lifecycle::shadow);
        CHECK(current_lifecycle("multi_timeframe_momentum_alignment", no_overrides, declared) ==
              Lifecycle::candidate);
        CHECK(current_lifecycle("momentum_continuation", no_overrides, declared) ==
              Lifecycle::active);
    }

    SECTION("an override wins over the declared status") {
        const std::map<std::string, Lifecycle> overrides{
            {"donchian_breakout_continuation", Lifecycle::degraded}};
        CHECK(current_lifecycle("donchian_breakout_continuation", overrides, declared) ==
              Lifecycle::degraded);
    }

    SECTION("an unknown strategy stays on the branch that can only demote") {
        CHECK(current_lifecycle("not_in_the_registry", {}, declared) == Lifecycle::active);
    }

    // The consequence the defect had: a SHADOW strategy with a clean record is
    // promotable, and reading it as ACTIVE silently removed that.
    SECTION("the ladder is reachable from the declared status") {
        LifecycleInputs in;
        in.drift = DriftStatus::stable;
        in.posterior_edge = 0.01;
        in.posterior_uncertainty = 0.005;
        in.sample_count = 150;
        in.min_samples = 100;
        in.cooldown_elapsed = true;

        in.current = current_lifecycle("donchian_breakout_continuation", {}, declared);
        const auto promoted = evaluate_lifecycle("donchian_breakout_continuation", "1", in, 100);
        REQUIRE(promoted.has_value());
        CHECK(promoted->to_status == Lifecycle::candidate);

        // Read as ACTIVE — what the pass used to do — the same evidence yields
        // no transition at all.
        in.current = Lifecycle::active;
        CHECK_FALSE(evaluate_lifecycle("donchian_breakout_continuation", "1", in, 100).has_value());
    }

    // Promotion still needs the evidence: the fix restores a path, it does not
    // manufacture a verdict.
    SECTION("a negative edge is not promoted, whatever the status resolves to") {
        LifecycleInputs in;
        in.drift = DriftStatus::stable;
        in.posterior_edge = -0.0027;  // the production reading for this cell
        in.posterior_uncertainty = 0.005;
        in.sample_count = 150;
        in.min_samples = 100;
        in.cooldown_elapsed = true;
        in.current = current_lifecycle("donchian_breakout_continuation", {}, declared);
        CHECK_FALSE(evaluate_lifecycle("donchian_breakout_continuation", "1", in, 100).has_value());
    }
}

TEST_CASE("candidate market outcome: taken from the last close BEFORE the candidate, both sides",
          "[outcomes][market][leakage]") {
    using namespace altstrategy::engine;
    // 1m bars from t=0; the candidate is raised at 10:30 into minute 10, so the
    // reference is the close of the bar that closed at minute 10 (opened at 9).
    std::vector<PathBar> bars;
    for (int i = 0; i < 260; ++i) {
        PathBar b;
        b.open_ms = i * 60'000LL;
        b.close = 100.0 + (i >= 10 ? 0.01 * (i - 9) : 0.0);
        b.open = b.close;
        b.high = b.close + 0.05;
        b.low = b.close - 0.05;
        // The bar the candidate is raised INSIDE has a wild range: it had not
        // closed, so it must not become the entry.
        if (i == 10) { b.high = 150.0; b.low = 50.0; }
        bars.push_back(b);
    }
    const int64_t created = 10 * 60'000LL + 30'000;
    const auto l = candidate_market_outcomes("c1", Side::long_, created, bars, 16.0);
    REQUIRE(l.size() == 3);
    CHECK(l[0].entry_ref_ms == 10 * 60'000LL);
    CHECK(l[0].entry_price == Catch::Approx(100.0));            // close of bar 9
    CHECK(l[0].exit_price == Catch::Approx(100.0 + 0.01 * 15)); // close of bar 24
    CHECK(l[0].gross_bps == Catch::Approx(15.0));
    REQUIRE(l[0].net_bps.has_value());
    CHECK(*l[0].net_bps == Catch::Approx(-1.0));
    // MFE/MAE read the bars AFTER the reference -- which includes bar 10's range.
    CHECK(l[0].mfe_bps > 4000.0);
    const auto s = candidate_market_outcomes("c1", Side::short_, created, bars, std::nullopt);
    REQUIRE(s.size() == 3);
    CHECK(s[0].gross_bps == Catch::Approx(-15.0));
    CHECK_FALSE(s[0].net_bps.has_value());                        // no model cost: no net, not 0
    // A missing minute removes the horizons that span it, and only those.
    bars.erase(bars.begin() + 100);
    const auto g = candidate_market_outcomes("c1", Side::long_, created, bars, 16.0);
    REQUIRE(g.size() == 2);                                     // minute 100 is inside 4h only
    CHECK(g[0].horizon_minutes == 15);
    CHECK(g[1].horizon_minutes == 60);
}

TEST_CASE("candidate market outcome: only a 1m path, and when the extremes happened",
          "[outcomes][market][regression]") {
    using namespace altstrategy::engine;
    // A path from 1-second trade buckets used to be read as minutes: 1h / 4h
    // found a "missing minute" and were dropped for good, 15m was built from
    // fifteen single seconds. A non-minute bar is now refused outright.
    std::vector<PathBar> secs;
    for (int i = 0; i < 20000; ++i) {
        PathBar b;
        b.open_ms = i * 1'000LL;
        b.open = b.close = 100.0;
        b.high = 100.01;
        b.low = 99.99;
        secs.push_back(b);
    }
    CHECK(candidate_market_outcomes("c1", Side::long_, 600'000 + 500, secs, 16.0).empty());

    // Minute path: the high at minute 20 after the reference, the low at 5.
    std::vector<PathBar> bars;
    for (int i = 0; i < 260; ++i) {
        PathBar b;
        b.open_ms = i * 60'000LL;
        b.open = b.close = 100.0;
        b.high = 100.1;
        b.low = 99.9;
        if (i == 10 + 19) b.high = 101.0;   // the 20th bar after the reference close
        if (i == 10 + 4) b.low = 99.0;      // the 5th
        bars.push_back(b);
    }
    const int64_t created = 10 * 60'000LL + 30'000;
    const auto l = candidate_market_outcomes("c1", Side::long_, created, bars, 16.0);
    REQUIRE(l.size() == 3);
    CHECK(l[0].horizon_minutes == 15);
    CHECK(l[0].t_mae_min == 5);        // inside 15m
    CHECK(l[1].horizon_minutes == 60);
    CHECK(l[1].t_mfe_min == 20);
    CHECK(l[1].t_mae_min == 5);
    const auto s = candidate_market_outcomes("c1", Side::short_, created, bars, 16.0);
    CHECK(s[1].t_mfe_min == 5);        // a short's favourable extreme is the low
    CHECK(s[1].t_mae_min == 20);
}

TEST_CASE("lifecycle: ACTIVE leaves on measured loss without drift; RESEARCH has an exit",
          "[lifecycle]") {
    using namespace altstrategy::engine;
    LifecycleInputs in;
    in.current = Lifecycle::active;
    in.drift = DriftStatus::stable;          // the detector sees nothing
    in.posterior_edge = -0.0008;             // -8 bps, measured
    in.sample_count = 150;
    in.min_samples = 100;
    const auto t = evaluate_lifecycle("s", "1", in, 100);
    REQUIRE(t.has_value());
    CHECK(t->to_status == Lifecycle::degraded);
    CHECK(t->reason == "measured_negative_edge");
    // Too few samples to call it measured: stays.
    in.sample_count = 40;
    CHECK_FALSE(evaluate_lifecycle("s", "1", in, 100).has_value());

    LifecycleInputs r;
    r.current = Lifecycle::research;
    r.drift = DriftStatus::stable;
    r.posterior_edge = 0.0012;
    r.posterior_uncertainty = 0.002;
    r.sample_count = 120;
    r.min_samples = 100;
    const auto up = evaluate_lifecycle("s", "1", r, 100);
    REQUIRE(up.has_value());
    CHECK(up->to_status == Lifecycle::shadow);
    // Negative evidence keeps it collecting data in RESEARCH -- never promoted.
    r.posterior_edge = -0.001;
    CHECK_FALSE(evaluate_lifecycle("s", "1", r, 100).has_value());
}
