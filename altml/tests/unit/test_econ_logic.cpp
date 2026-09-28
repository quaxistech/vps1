// The economic target, calibration and decision arithmetic.

#include "altml/components/econ_calibration.hpp"
#include "altml/components/econ_logic.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>

using namespace altml;
using Catch::Approx;

namespace {

labels::SourceBar bar(int64_t minute, double o, double h, double l, double c) {
    return {minute * 60'000, o, h, l, c, 0, true};
}

} // namespace

TEST_CASE("the class boundary is the all-in cost and nothing else", "[econ][label]") {
    const double cost = econ::cost_from_fee_model(0.0006, 2.0).fraction();
    REQUIRE(cost == Approx(0.0016));
    // A move that a direction label calls "up" but that does not pay for the
    // round trip is NEUTRAL: +2 bps against a 16 bps cost.
    CHECK(econ::econ_class(std::log1p(0.0002), cost) == econ::kNeutral);
    CHECK(econ::econ_class(std::log1p(-0.0002), cost) == econ::kNeutral);
    // Just past the cost on either side.
    CHECK(econ::econ_class(std::log1p(0.00161), cost) == econ::kLong);
    CHECK(econ::econ_class(std::log1p(-0.00161), cost) == econ::kShort);
    // Just short of the cost does not pay: a trade that returns its own cost
    // and a hair less is a loss, not a neutral-ish win.
    CHECK(econ::econ_class(std::log1p(0.00159), cost) == econ::kNeutral);
    CHECK(econ::econ_class(std::log1p(-0.00159), cost) == econ::kNeutral);
}

TEST_CASE("the path outcome reads only bars after the anchor", "[econ][label][leakage]") {
    // The anchor bar's own high and low are NOT part of the future: a spike
    // inside the bar that closes at T0 happened before the decision.
    std::vector<labels::SourceBar> bars = {
        bar(0, 100, 150, 50, 100),   // anchor: extreme range, must be ignored
        bar(1, 100, 101, 99.5, 100.5),
        bar(2, 100.5, 102, 100, 101),
        bar(3, 101, 101.5, 98, 99),
    };
    REQUIRE(labels::source_range_valid(bars, 0, 3));
    const auto o = econ::path_outcome(bars, 0, 3);
    CHECK(o.log_return == Approx(std::log(99.0 / 100.0)));
    CHECK(o.mfe_long == Approx(std::log(102.0 / 100.0)));
    CHECK(o.mae_long == Approx(std::log(98.0 / 100.0)));
    CHECK(o.minutes_to_mfe == 2);
    CHECK(o.minutes_to_mae == 3);
}

TEST_CASE("an incomplete horizon has no outcome", "[econ][label][boundary]") {
    std::vector<labels::SourceBar> bars = {bar(0, 1, 1, 1, 1), bar(1, 1, 1, 1, 1), bar(3, 1, 1, 1, 1)};
    // Minute 2 is missing: a 2-minute path from the anchor is not a path.
    CHECK_FALSE(labels::source_range_valid(bars, 0, 2));
    // And a horizon past the end of the data is not labelable yet.
    CHECK_FALSE(labels::source_range_valid(bars, 0, 5));
}

TEST_CASE("temperature scaling recovers an over-confident model", "[econ][calibration]") {
    // Margins that are three times too sharp: the true probabilities come from
    // softmax(z / 3). The fitted T must land near 3, and the NLL must improve.
    std::mt19937 rng(7);
    std::normal_distribution<double> nz(0.0, 1.0);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<double> raw;
    std::vector<int> y;
    for (int i = 0; i < 20000; ++i) {
        const double z[3] = {3 * nz(rng), 3 * nz(rng), 3 * nz(rng)};
        const auto p = econ::softmax3(z, 3.0);
        const double r = u(rng);
        y.push_back(r < p[0] ? 0 : (r < p[0] + p[1] ? 1 : 2));
        raw.insert(raw.end(), z, z + 3);
    }
    const double t = econ::fit_temperature(raw, y);
    CHECK(t == Approx(3.0).margin(0.25));
    CHECK(econ::nll_at(raw, y, t) < econ::nll_at(raw, y, 1.0));
}

TEST_CASE("proper scores: a perfect forecast beats climatology, climatology has zero skill",
          "[econ][metrics]") {
    const std::vector<int> y = {0, 1, 2, 2, 1, 0, 2, 2};
    std::vector<int64_t> idx(y.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<int64_t>(i);
    const auto freq = econ::class_frequencies(y, idx);
    CHECK(freq[2] == Approx(0.5));
    const auto clim = econ::climatology(freq, y.size());
    std::vector<double> perfect(y.size() * 3, 0.0);
    for (size_t i = 0; i < y.size(); ++i) perfect[i * 3 + static_cast<size_t>(y[i])] = 1.0;
    CHECK(econ::multi_brier(perfect, y) == Approx(0.0));
    CHECK(econ::multi_logloss(perfect, y) < econ::multi_logloss(clim, y));
    CHECK(econ::ece_class(perfect, y, 2) == Approx(0.0));
}

TEST_CASE("expectation and decision: high cost, small move, large move", "[econ][policy]") {
    const double cost = 0.0016;
    econ::ClassMeans m;
    m.ret = {-0.006, 0.0001, 0.006};   // what SHORT / NEUTRAL / LONG rows were worth
    m.mfe = {0.001, 0.002, 0.008};
    m.mae = {-0.008, -0.002, -0.001};
    m.n = {100, 800, 100};

    SECTION("NEUTRAL: most mass on no move -> no side clears the cost") {
        const auto e = econ::estimate({0.1, 0.8, 0.1}, m, cost);
        CHECK(e.expected_return == Approx(0.00008));
        CHECK(econ::decide_side(e) == 0);
    }
    SECTION("LONG: a large expected move clears the cost") {
        const auto e = econ::estimate({0.05, 0.35, 0.60}, m, cost);
        CHECK(e.expected_net_long > 0);
        CHECK(econ::decide_side(e) == 1);
    }
    SECTION("SHORT is symmetric") {
        const auto e = econ::estimate({0.60, 0.35, 0.05}, m, cost);
        CHECK(e.expected_net_short > 0);
        CHECK(econ::decide_side(e) == -1);
    }
    SECTION("a favourable probability does not overcome a high cost") {
        // P(long) clearly above P(short) but the expected move is small
        // against the round trip: no trade. Probability is not a decision.
        const auto e = econ::estimate({0.2, 0.5, 0.3}, m, 0.01);
        CHECK(e.p_long > e.p_short);
        CHECK(econ::decide_side(e) == 0);
    }
    SECTION("both sides are never positive at once") {
        const auto e = econ::estimate({0.34, 0.32, 0.34}, m, cost);
        CHECK(e.expected_net_long + e.expected_net_short == Approx(-2 * cost));
    }
}

TEST_CASE("one open position per instrument, overlapping across instruments",
          "[econ][replay]") {
    const std::vector<int64_t> t = {0, 5, 15, 16, 0, 5};
    const std::vector<int32_t> sym = {1, 1, 1, 1, 2, 2};
    const std::vector<int> side = {1, 1, -1, 1, -1, 0};
    const std::vector<int64_t> idx = {0, 1, 2, 3, 4, 5};
    const auto sel = econ::select_per_instrument(idx, t, sym, side, 15);
    // instrument 1: 0 taken, 5 inside the hold, 15 free again, 16 inside;
    // instrument 2: 0 taken, 5 has no side.
    REQUIRE(sel.size() == 3);
    CHECK(sel[0] == 0);
    CHECK(sel[1] == 4);
    CHECK(sel[2] == 2);
}

TEST_CASE("realised economics charge the cost on every trade", "[econ][replay]") {
    const std::vector<int64_t> sel = {0, 1};
    const std::vector<int> side = {1, -1};
    const std::vector<double> r = {std::log1p(0.01), std::log1p(0.01)};
    const std::vector<double> mfe = {std::log1p(0.012), std::log1p(0.012)};
    const std::vector<double> mae = {std::log1p(-0.002), std::log1p(-0.002)};
    const std::vector<int64_t> t = {0, 1};
    const auto e = econ::evaluate(sel, side, r, mfe, mae, t, 0.0016);
    CHECK(e.trades == 2);
    CHECK(e.mean_gross == Approx(0.0));             // +1% long, -1% short
    CHECK(e.mean_net == Approx(-0.0016));
    CHECK(e.long_share == Approx(0.5));
    CHECK(e.mean_mfe == Approx((0.012 + 0.002) / 2)); // short's favourable = -mae
}

TEST_CASE("VPS1 promotes on market prediction quality; the economics gate nothing",
          "[econ][gates][vps1][regression]") {
    // The policy-2 economic gates rejected every model on 2026-09-25 although
    // each predicted the market better than the base rates. A model that
    // knows WHEN the market moves (positive proper-score skill) is market
    // intelligence; whether acting on it pays is VPS2's question.
    econ::SliceScore fold;
    fold.logloss_skill = 0.05;
    fold.brier_skill = 0.04;
    fold.econ.trades = 0;                 // no position would ever be taken
    fold.econ.mean_net = -0.0020;
    std::vector<econ::SliceScore> folds(4, fold);
    CHECK(econ::market_gate_failures(folds, fold, econ::MarketGateConfig{}).empty());

    // No better than the base rates on the holdout: refused.
    econ::SliceScore flat = fold;
    flat.logloss_skill = -0.001;
    CHECK_FALSE(econ::market_gate_failures(folds, flat, econ::MarketGateConfig{}).empty());
    // Brier must agree.
    econ::SliceScore brier = fold;
    brier.brier_skill = -0.001;
    CHECK_FALSE(econ::market_gate_failures(folds, brier, econ::MarketGateConfig{}).empty());
    // One lucky month is not consistency: two of four folds beating the base
    // rates fails, three passes.
    std::vector<econ::SliceScore> mixed(4, fold);
    mixed[0].logloss_skill = mixed[1].logloss_skill = -0.01;
    CHECK_FALSE(econ::market_gate_failures(mixed, fold, econ::MarketGateConfig{}).empty());
    mixed[1].logloss_skill = 0.02;
    CHECK(econ::market_gate_failures(mixed, fold, econ::MarketGateConfig{}).empty());
    CHECK_FALSE(econ::market_gate_failures({}, fold, econ::MarketGateConfig{}).empty());
}

TEST_CASE("calibration test: a calibrated forecast passes, a shifted one does not, deterministically",
          "[econ][calibration][vps1]") {
    std::mt19937_64 rng(11);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const size_t n = 20'000;
    std::vector<double> p(n * 3);
    std::vector<int> y(n);
    for (size_t i = 0; i < n; ++i) {
        const double up = 0.15 + 0.3 * u(rng), down = 0.15 + 0.3 * u(rng);
        const double flat = 1.0 - up - down;
        p[i * 3] = down; p[i * 3 + 1] = flat; p[i * 3 + 2] = up;
        const double x = u(rng);
        y[i] = x < down ? 0 : (x < down + flat ? 1 : 2);   // drawn from the forecast itself
    }
    const auto ok = econ::calibration_test(p, y);
    CHECK(ok.passed);
    CHECK(ok.sims == 200);
    for (size_t k = 0; k < 3; ++k) CHECK(ok.ece[k] <= ok.null_q95[k]);
    const auto again = econ::calibration_test(p, y);
    CHECK(again.null_q95 == ok.null_q95);               // same seed, same verdict
    CHECK(again.ece == ok.ece);

    // The same outcomes against a forecast that overstates "up" by 0.08.
    std::vector<double> q = p;
    for (size_t i = 0; i < n; ++i) { q[i * 3 + 2] += 0.08; q[i * 3 + 1] -= 0.08; }
    CHECK_FALSE(econ::calibration_test(q, y).passed);
    // Too few rows to test: not passed (no claim is made).
    CHECK_FALSE(econ::calibration_test(std::vector<double>(30, 1.0 / 3), std::vector<int>(10, 1)).passed);
}

TEST_CASE("gates refuse a model that only forecasts volatility (policy-2, now a diagnostic)", "[econ][gates]") {
    // Positive proper-score skill (it knows WHEN the market moves) but no
    // economic edge (it does not know WHICH WAY): the ECONOMIC verdict says
    // so, and is reported beside the model; it no longer decides promotion.
    econ::SliceScore fold;
    fold.logloss_skill = 0.08;
    fold.brier_skill = 0.09;
    fold.econ.trades = 200;
    fold.econ.mean_net = -0.0020;
    fold.econ.t_day = -1.2;
    std::vector<econ::SliceScore> folds(4, fold);
    const auto f = econ::gate_failures(folds, fold, econ::GateConfig{});
    CHECK_FALSE(f.empty());

    // A model that clears every gate.
    econ::SliceScore good = fold;
    good.econ.mean_net = 0.0010;
    good.econ.t_day = 2.5;
    std::vector<econ::SliceScore> good_folds(4, good);
    CHECK(econ::gate_failures(good_folds, good, econ::GateConfig{}).empty());

    // Too few holdout trades is a refusal however good the mean looks.
    econ::SliceScore thin = good;
    thin.econ.trades = 12;
    CHECK_FALSE(econ::gate_failures(good_folds, thin, econ::GateConfig{}).empty());
}

TEST_CASE("cost v2: fee and impact are fixed, the spread is the one observed at T0",
          "[econ][cost]") {
    const econ::CostModelV2 m;
    const auto liquid = m.at(0.02);
    CHECK(liquid.spread_observed);
    CHECK(liquid.bps() == Approx(12.0 + 0.02 + 2.0));
    const auto thin = m.at(7.0);
    CHECK(thin.bps() == Approx(21.0));
    // No reading, or a broken one: the stated fallback, and it says so.
    for (const double bad : {std::nan(""), 0.0, -1.0, 500.0}) {
        const auto c = m.at(bad);
        CHECK_FALSE(c.spread_observed);
        CHECK(c.spread_bps == Approx(m.fallback_spread_bps));
    }
}

TEST_CASE("the policy margin is chosen on validation and needs enough trades",
          "[econ][policy][margin]") {
    // 60 rows on 60 instruments (no overlap), estimated net +8 bps long for
    // all; realised +30 bps on half, -10 bps on the other half.
    const size_t n = 60;
    std::vector<int64_t> idx(n), t(n, 0);
    std::vector<int32_t> sym(n);
    std::vector<double> r(n), mfe(n, 0.0), mae(n, 0.0), cost(n, 0.0016);
    std::vector<econ::Estimate> est(n);
    for (size_t i = 0; i < n; ++i) {
        idx[i] = static_cast<int64_t>(i);
        sym[i] = static_cast<int32_t>(i);
        r[i] = std::log1p(i % 2 == 0 ? 0.0046 : 0.0006);
        est[i].expected_net_long = 0.0008;
        est[i].expected_net_short = -0.0040;
    }
    const auto c = econ::select_margin(idx, est, t, sym, r, mfe, mae, cost, 900'000);
    REQUIRE(c.margin_bps.has_value());
    // margins 0/2/5 keep all 60 trades (8 bps > margin), 10/20 keep none
    CHECK(*c.margin_bps == Approx(0.0));
    CHECK(c.trades == 60);
    CHECK(c.mean_net_bps == Approx(((46.0 - 16.0) + (6.0 - 16.0)) / 2.0).margin(1e-6));

    // Every margin loses on validation: no margin, however many trades.
    std::vector<double> losing(n, std::log1p(0.0006));   // +6 bps gross vs 16 bps cost
    CHECK_FALSE(econ::select_margin(idx, est, t, sym, losing, mfe, mae, cost, 900'000).margin_bps);

    // Fewer than kMinValidationTrades possible trades: no margin at all.
    const std::vector<int64_t> few(idx.begin(), idx.begin() + 20);
    const std::vector<econ::Estimate> few_est(est.begin(), est.begin() + 20);
    CHECK_FALSE(econ::select_margin(few, few_est, t, sym, r, mfe, mae, cost, 900'000).margin_bps);
}

TEST_CASE("a margin excludes estimates that clear the cost only by a hair", "[econ][policy][margin]") {
    econ::Estimate e;
    e.expected_net_long = 0.00003;   // +0.3 bps
    e.expected_net_short = -0.0032;
    CHECK(econ::decide_side(e, 0.0) == 1);
    CHECK(econ::decide_side(e, 0.0002) == 0);
}

namespace {

// The calibration record of 20260925T193754Z-lightgbm-econ1h-6ab21e9f as the
// VPS1 registry holds it (descriptive strings trimmed). Replay crashed on it.
nlohmann::json policy2_record() {
    return nlohmann::json::parse(R"({
      "policy": {"version": "policy-2", "margin_bps": 0.0, "validation_trades": 160,
                 "validation_net_bps": -7.121157005178715, "min_validation_trades": 30,
                 "margin_grid_bps": [0.0, 2.0, 5.0, 10.0, 20.0]},
      "label_set": "econ-labels:1", "calibrator": "temperature-1",
      "cost_model": {"version": "cost-v2", "fee_bps_per_side": 5.999999999999999,
                     "fallback_spread_bps": 2.0, "impact_bps_per_side": 1.0,
                     "max_plausible_spread_bps": 200.0},
      "class_order": ["SHORT", "NEUTRAL", "LONG"], "temperature": 0.8991524542691186,
      "horizon_minutes": 60, "class_counts_fit": [72242, 49277, 75943], "fee_model_version": 1,
      "class_means_return": [-0.007402681119772842, 0.000010281999039357989, 0.008075723956573235],
      "class_means_mae_long": [-0.011281447603271953, -0.0035351560772009898, -0.0029368763476770644],
      "class_means_mfe_long": [0.002921169148264066, 0.0034754101577794917, 0.011717661741130035]})");
}

} // namespace

TEST_CASE("calibration: a policy-2 record parses, and a margin that lost on validation is dropped",
          "[econ][calibration][regression]") {
    auto rec = policy2_record();
    econ::CalibrationError err;
    const auto c = econ::parse_calibration(rec, &err);
    REQUIRE(c.has_value());
    CHECK(c->horizon_minutes == 60);
    CHECK(c->temperature == Approx(0.8991524542691186));
    CHECK(c->means.ret[2] == Approx(0.008075723956573235));
    CHECK(c->cost_model.fee_bps_per_side == Approx(6.0));
    CHECK(c->policy_version == "policy-2");
    CHECK_FALSE(c->margin_bps.has_value());   // validation net -7.1 bps: not a margin

    rec["policy"]["validation_net_bps"] = 3.5;
    rec["policy"]["margin_bps"] = 5.0;
    const auto paid = econ::parse_calibration(rec);
    REQUIRE(paid.has_value());
    REQUIRE(paid->margin_bps.has_value());
    CHECK(*paid->margin_bps == Approx(5.0));

    rec["policy"]["margin_bps"] = nullptr;    // no margin chosen at all
    const auto none = econ::parse_calibration(rec);
    REQUIRE(none.has_value());
    CHECK_FALSE(none->margin_bps.has_value());
}

TEST_CASE("calibration: probabilities are called calibrated only on a passed holdout test",
          "[econ][calibration][vps1]") {
    auto rec = policy2_record();
    auto c = econ::parse_calibration(rec);
    REQUIRE(c.has_value());
    CHECK_FALSE(c->probability_calibrated);             // a record from before the test: no claim
    CHECK(c->calibration_version == "temperature-1");
    CHECK(c->label_definition.rfind("econ-class:cost-v1", 0) == 0);
    rec["calibration_test"] = {{"version", "ece-null-q95-1"}, {"passed", true}};
    rec["label_definition"] = "econ-class:cost-v2 -- up / down beyond the round trip";
    c = econ::parse_calibration(rec);
    REQUIRE(c.has_value());
    CHECK(c->probability_calibrated);
    CHECK(c->label_definition.rfind("econ-class:cost-v2", 0) == 0);
    rec["calibration_test"]["passed"] = "yes";          // not a boolean: no claim
    CHECK_FALSE(econ::parse_calibration(rec)->probability_calibrated);
    rec["calibration_test"]["passed"] = false;
    CHECK_FALSE(econ::parse_calibration(rec)->probability_calibrated);
}

TEST_CASE("calibration: a policy-1 record still loads, with no margin", "[econ][calibration]") {
    auto rec = policy2_record();
    rec.erase("policy");
    rec.erase("cost_model");
    rec["round_trip_cost_bps"] = 16.0;
    const auto c = econ::parse_calibration(rec);
    REQUIRE(c.has_value());
    CHECK(c->policy_version == "policy-1");
    CHECK_FALSE(c->margin_bps.has_value());
    CHECK(c->cost_model.fee_bps_per_side == Approx(6.0));   // decision cost is v2 regardless
}

TEST_CASE("calibration: a malformed record is refused with a reason, never thrown",
          "[econ][calibration][regression]") {
    const auto refused = [](nlohmann::json rec, const std::string& why) {
        econ::CalibrationError err;
        std::optional<econ::Calibration> c;
        REQUIRE_NOTHROW(c = econ::parse_calibration(rec, &err));
        CHECK_FALSE(c.has_value());
        CHECK(err.reason == why);
    };
    auto rec = policy2_record();
    refused(nlohmann::json::parse("{not json", nullptr, false), "calibration_incomplete");
    refused(nlohmann::json::array(), "calibration_incomplete");
    { auto r = rec; r.erase("cost_model"); refused(r, "calibration_incomplete"); }
    { auto r = rec; r["class_means_return"] = {0.1, 0.2}; refused(r, "calibration_incomplete"); }
    { auto r = rec; r["class_means_mfe_long"][1] = "x"; refused(r, "calibration_incomplete"); }
    { auto r = rec; r["temperature"] = "0.9"; refused(r, "calibration_incomplete"); }
    { auto r = rec; r["horizon_minutes"] = 60.5; refused(r, "calibration_incomplete"); }
    { auto r = rec; r["calibrator"] = "isotonic-1"; refused(r, "calibration_incomplete"); }
    { auto r = rec; r["temperature"] = 0.0; refused(r, "calibration_inconsistent"); }
    { auto r = rec; r["cost_model"]["version"] = "cost-v9"; refused(r, "cost_model_unknown"); }
    { auto r = rec; r["policy"]["margin_bps"] = "5"; r["policy"]["validation_net_bps"] = 1.0;
      const auto c = econ::parse_calibration(r);   // a non-numeric margin is no margin
      REQUIRE(c.has_value());
      CHECK_FALSE(c->margin_bps.has_value()); }
}

TEST_CASE("rank statistics: Spearman with ties and gaps, direction AUC", "[econ][magnitude][vps1]") {
    // Monotone -> 1, reversed -> -1, NaN pairs ignored, ties averaged.
    CHECK(econ::spearman({1, 2, 3, 4}, {10, 20, 30, 40}) == Approx(1.0));
    CHECK(econ::spearman({1, 2, 3, 4}, {4, 3, 2, 1}) == Approx(-1.0));
    CHECK(econ::spearman({1, 2, std::nan(""), 4, 5}, {1, 2, 99, 4, 5}) == Approx(1.0));
    const auto r = econ::average_ranks({5, 1, 5, 3});
    CHECK(r[0] == Approx(3.5));
    CHECK(r[2] == Approx(3.5));
    CHECK(r[1] == Approx(1.0));
    CHECK(std::isnan(econ::spearman({1, 2}, {1, 2})));

    // Direction AUC: P(long) - P(short) ranks the up-moves first -> 1; no
    // information -> 0.5; zero returns do not count.
    std::vector<double> p = {0.1, 0.3, 0.6,   0.6, 0.3, 0.1,   0.2, 0.3, 0.5,   0.5, 0.3, 0.2,  0.3, 0.4, 0.3};
    std::vector<double> ret = {0.01, -0.01, 0.02, -0.02, 0.0};
    CHECK(econ::direction_auc(p, ret) == Approx(1.0));
    std::vector<double> flat(15, 1.0 / 3);
    CHECK(econ::direction_auc(flat, ret) == Approx(0.5));
    CHECK(std::isnan(econ::direction_auc(p, {0.01, 0.02, 0.03, 0.01, 0.0})));   // one class only
}

TEST_CASE("calibration: the magnitude source is read from the record, class means by default",
          "[econ][calibration][magnitude]") {
    auto rec = policy2_record();
    CHECK(econ::parse_calibration(rec)->magnitude_source == "class_means");
    rec["magnitude_source"] = "regression";
    CHECK(econ::parse_calibration(rec)->magnitude_source == "regression");
}
