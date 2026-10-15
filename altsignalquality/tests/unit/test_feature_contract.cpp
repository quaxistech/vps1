// The feature contract is the service's identity: order, names, sources and the
// hash over them decide what every stored vector and every shipped model MEANS.
// These tests pin exactly the properties that, if they broke, would be invisible
// in production.

#include "altsignalquality/domain/feature_contract.hpp"

#include "altsignalquality/core/hash.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <nlohmann/json.hpp>

#include <set>

using namespace altsignalquality::domain;

namespace {

// A snapshot with everything present, so a test can knock out one input at a
// time and see exactly which position goes missing.
SignalSnapshot full_snapshot() {
    SignalSnapshot s;
    auto& c = s.candidate;
    c.signal_proposal_id = "sig-1";
    c.candidate_id = "cand-1";
    c.canonical_pair_id = 42;
    c.strategy_id = "momentum_continuation";
    c.side = 1;  // LONG
    c.proposal_grade = "NORMAL";
    c.plan_version = 1;
    c.holding_class = 2;
    c.quality = "STRICT_4_OF_4";
    c.created_at_ms = 1'770'000'000'000;
    c.valid_until_ms = c.created_at_ms + 900'000;
    c.entry_type = 2;
    c.entry_price = 100.0;
    c.stop_loss_price = 98.0;
    c.take_profit_price = 104.0;
    c.stop_distance_bps = 200;
    c.stop_distance_atr = 1.8;
    c.target_distance_bps = 400;
    c.max_slippage_bps = 6;
    c.expected_fill_time_p50_min = 3;
    c.expected_fill_time_p90_min = 9;
    c.entry_fill_timeout_ms = 7'200'000;
    c.expected_holding_time_p50_ms = 3'600'000;
    c.expected_holding_time_p90_ms = 10'800'000;
    c.max_holding_time_ms = 172'800'000;
    c.time_stop_at_ms = c.created_at_ms + 172'800'000;
    c.edge_estimated = true;
    c.p_target_before_stop = 0.45;
    c.expected_net_return = 0.003;
    c.expected_rr = 2.0;
    c.ml_conviction = 0.3;
    c.strategy_confidence = 0.7;
    c.ensemble_agreement = 0.6;
    c.adaptive_score = 0.9;
    c.recommended_for_execution = true;

    auto& x = s.context;
    x.present = true;
    x.event_time_ms = c.created_at_ms - 60'000;
    x.available_at_ms = c.created_at_ms - 30'000;
    x.data_quality = "STRICT_4_OF_4";
    x.bitget_regime = "TREND_UP";
    x.global_regime = "TREND_UP";
    x.venue_mask = 0b1111;
    x.missing_venue_mask = 0;
    x.fees_bps = 6.0;
    x.funding_rate = 0.0001;
    x.mark_price = 100.5;
    x.spread_bps = 2.0;
    x.top_depth_usdt = 250'000;
    x.liquidity_rank = 0.8;
    x.expected_slippage_bps = 3.0;
    x.bitget_features = {{"atr_14", 1.5},   {"atr_50", 1.2},        {"adx_14", 28.0},
                         {"rsi_14", 61.0},  {"roc_14", 1.2},        {"mom_14", 0.9},
                         {"bb_upper_20", 103.0}, {"bb_lower_20", 97.0}, {"bb_middle_20", 100.0},
                         {"ema_20", 100.2}, {"ema_50", 99.4},       {"ema_200", 96.0},
                         {"donchian_high_20", 105.0}, {"donchian_low_20", 95.0},
                         {"stddev_20", 1.1}};
    x.microstructure = {{"trade_imbalance", 0.2},   {"book_imbalance", 0.1},
                        {"cvd", 1200.0},            {"cvd_delta", 30.0},
                        {"trade_intensity", 4.0},   {"signed_volume", 900.0},
                        {"turnover_acceleration", 1.1}, {"book_microprice_dev", 0.4},
                        {"book_bid_depth", 60'000}, {"book_ask_depth", 40'000}};
    x.cross_venue = {{"dislocation_bps", 3.0},        {"venue_price_dispersion_bps", 1.5},
                     {"momentum_agreement", 0.7},     {"breakout_agreement", 0.5},
                     {"funding_divergence", 0.02},    {"oi_growth", 0.03},
                     {"cross_venue_confidence", 0.9}, {"rsi_dispersion", 2.0},
                     {"volatility_agreement", 0.8},   {"trend_alignment_agreement", 0.6}};
    // Higher-timeframe context: the shortest grid AltStrategy admitted.
    x.htf_minutes = 30;
    x.htf_features = {{"adx_14", 24.0}, {"rsi_14", 57.0},
                      {"ema_20", 101.2}, {"ema_50", 100.4}};
    x.regime_confidence = 0.75;
    x.regime_trend_strength = 0.5;
    x.regime_volatility = 0.4;
    x.regime_liquidity_quality = 0.9;

    auto& p = s.priors;
    for (PriorCell* cell : {&p.global, &p.strategy, &p.pair, &p.regime, &p.strategy_regime,
                            &p.strategy_side, &p.grade, &p.decayed_strategy}) {
        cell->present = true;
        cell->win_rate = 0.6;
        cell->expectancy = 0.002;
        cell->effective_samples = 40;
    }
    return s;
}

size_t position_of(std::string_view name) {
    for (size_t i = 0; i < kFeatures.size(); ++i) {
        if (kFeatures[i].name == name) return i;
    }
    FAIL("feature not in the contract: " << name);
    return 0;
}

} // namespace

TEST_CASE("every contract member is declared knowable at signal time") {
    for (const auto& f : kFeatures) {
        INFO("feature " << f.name);
        REQUIRE(f.availability == Availability::at_signal_time);
    }
}

TEST_CASE("contract member names are unique") {
    std::set<std::string_view> names;
    for (const auto& f : kFeatures) {
        INFO("feature " << f.name);
        REQUIRE(names.insert(f.name).second);
    }
    REQUIRE(names.size() == kFeatureCount);
}

TEST_CASE("the schema hash is stable and covers order, names, kinds and sources") {
    const std::string first = feature_schema_hash();
    REQUIRE(first == feature_schema_hash());
    REQUIRE(first.size() == 64);

    // The hash must be a function of the CONTRACT, not of the process: it is
    // recomputed here from the same canonical form the implementation uses, so
    // reordering two members would necessarily change it.
    std::string canonical(kFeatureSchemaVersion);
    canonical += '\n';
    for (size_t i = 0; i < kFeatures.size(); ++i) {
        canonical += std::to_string(i) + "|" + std::string(kFeatures[i].name) + "|" +
                     std::string(kind_name(kFeatures[i].kind)) + "|" +
                     std::string(source_name(kFeatures[i].source)) + "|" +
                     std::string(availability_name(kFeatures[i].availability)) + "\n";
    }
    REQUIRE(altsignalquality::sha256_hex(canonical) == first);
}

TEST_CASE("assembly is deterministic and produces exactly one value per member") {
    const auto s = full_snapshot();
    const auto a = assemble(s);
    const auto b = assemble(s);
    REQUIRE(a.values.size() == kFeatureCount);
    REQUIRE(a.validity.size() == (kFeatureCount + 7) / 8);
    REQUIRE(a.values == b.values);
    REQUIRE(a.validity == b.validity);
    REQUIRE(a.missing_count == b.missing_count);
}

TEST_CASE("a fully populated snapshot leaves nothing missing") {
    const auto v = assemble(full_snapshot());
    for (size_t i = 0; i < kFeatureCount; ++i) {
        INFO("position " << i << " = " << kFeatures[i].name);
        REQUIRE(v.observed(i));
    }
    REQUIRE(v.missing_count == 0);
}

TEST_CASE("a member the proposal declares absent is counted, but is not a missing input") {
    // AltStrategy's econ models produce no target-before-stop probability, so
    // p_target_before_stop -- and rr_times_p_target, built on it -- is absent on
    // every proposal, and MISSING_FEATURES was raised on every evaluation.
    auto s = full_snapshot();
    s.candidate.p_target_before_stop.reset();
    auto v = assemble(s);
    CHECK(v.missing_count == 2);
    CHECK(undeclared_missing_count(v, s.candidate) == 0);

    // No edge estimated: the economics are declared absent as well.
    s.candidate.edge_estimated = false;
    v = assemble(s);
    CHECK(v.missing_count == 4);
    CHECK(undeclared_missing_count(v, s.candidate) == 0);

    // An estimate that arrives unusable was not declared absent: it counts.
    s.candidate.edge_estimated = true;
    s.candidate.expected_net_return = std::numeric_limits<double>::quiet_NaN();
    v = assemble(s);
    CHECK(undeclared_missing_count(v, s.candidate) == 2);   // expected_net_return, net_edge_bps

    // So does every gap in the market context.
    s = full_snapshot();
    s.candidate.p_target_before_stop.reset();
    s.context = ContextSnapshot{};
    v = assemble(s);
    CHECK(undeclared_missing_count(v, s.candidate) == v.missing_count - 2);
    CHECK(undeclared_missing_count(v, s.candidate) > 0);
}

TEST_CASE("an absent context makes exactly the context-derived features missing") {
    auto s = full_snapshot();
    s.context = ContextSnapshot{};  // not present
    const auto v = assemble(s);

    REQUIRE(v.missing_count > 0);
    for (size_t i = 0; i < kFeatureCount; ++i) {
        const bool context_derived = kFeatures[i].source == FeatureSource::context_snapshot ||
                                     kFeatures[i].source == FeatureSource::context_analytics;
        INFO("position " << i << " = " << kFeatures[i].name);
        if (context_derived) {
            REQUIRE_FALSE(v.observed(i));
        }
    }
    // The candidate's own fields and the priors are unaffected: a missing market
    // context must not blank out information that never came from it.
    REQUIRE(v.observed(position_of("expected_rr")));
    REQUIRE(v.observed(position_of("prior_strategy_win_rate")));
}

TEST_CASE("a missing value is NaN with its validity bit cleared, never a zero") {
    auto s = full_snapshot();
    s.context.spread_bps.reset();
    const auto v = assemble(s);
    const size_t pos = position_of("spread_bps");
    REQUIRE_FALSE(v.observed(pos));
    REQUIRE(std::isnan(v.values[pos]));

    // Dropping spread also drops the cost ratio that depends on it: an
    // incomplete cost picture must not be reported as a cheaper venue. Those
    // two, and nothing else, are what a missing spread costs.
    const size_t cost = position_of("cost_to_target_ratio");
    REQUIRE_FALSE(v.observed(cost));
    REQUIRE(v.missing_count == 2);
}

TEST_CASE("an unknown categorical token becomes missing, not a sentinel category") {
    auto s = full_snapshot();
    s.context.global_regime = "SOMETHING_ALTTECH_ADDED_LATER";
    const auto v = assemble(s);
    const size_t pos = position_of("global_regime_ord");
    REQUIRE_FALSE(v.observed(pos));
    REQUIRE(std::isnan(v.values[pos]));
    // The known regime on the other axis is untouched.
    REQUIRE(v.observed(position_of("bitget_regime_ord")));
}

TEST_CASE("derived features use the arithmetic they claim to") {
    using Catch::Matchers::WithinRel;
    const auto s = full_snapshot();
    const auto v = assemble(s);

    REQUIRE_THAT(v.values[position_of("stop_to_target_ratio")], WithinRel(200.0 / 400.0, 1e-6));
    REQUIRE_THAT(v.values[position_of("net_edge_bps")], WithinRel(30.0, 1e-6));
    REQUIRE_THAT(v.values[position_of("rr_times_p_target")], WithinRel(0.9, 1e-6));
    // AltStrategy's cost model, not a blended re-derivation. full_snapshot()
    // uses a limit entry, so: fees 2+6, half-spread 1 paid once on the exit,
    // depth impact max(0, 3-1) = 2  ->  11 bps against a 400 bps target.
    REQUIRE_THAT(v.values[position_of("cost_to_target_ratio")], WithinRel(11.0 / 400.0, 1e-6));
    // entry 100.0 against a mark of 100.5 is 49.75 bps BETTER for a long.
    REQUIRE_THAT(v.values[position_of("entry_offset_bps")],
                 WithinRel(0.5 / 100.5 * 10'000.0, 1e-6));
    REQUIRE_THAT(v.values[position_of("atr_expansion")], WithinRel(1.5 / 1.2, 1e-6));
    REQUIRE_THAT(v.values[position_of("micro_bid_depth_share")], WithinRel(0.6, 1e-6));
    REQUIRE_THAT(v.values[position_of("donchian_position_20")],
                 WithinRel((100.5 - 95.0) / 10.0, 1e-6));
}

TEST_CASE("the signed entry offset flips with the side") {
    auto s = full_snapshot();
    s.candidate.side = 2;  // SHORT: entering below the mark is now WORSE
    const auto v = assemble(s);
    REQUIRE(v.values[position_of("entry_offset_bps")] < 0);
}

TEST_CASE("a zero or absent denominator yields missing, never an infinity") {
    auto s = full_snapshot();
    s.candidate.target_distance_bps = 0;
    s.context.mark_price = 0.0;
    const auto v = assemble(s);
    for (const char* name : {"stop_to_target_ratio", "cost_to_target_ratio", "entry_offset_bps",
                             "atr_14_bps", "price_vs_ema200_bps"}) {
        INFO(name);
        const size_t pos = position_of(name);
        REQUIRE_FALSE(v.observed(pos));
        REQUIRE(std::isnan(v.values[pos]));
    }
}

TEST_CASE("time of day is encoded on a circle so midnight is not an outlier") {
    using Catch::Matchers::WithinAbs;
    auto s = full_snapshot();
    const int64_t midnight = 1'770'000'000'000 / 86'400'000 * 86'400'000;
    s.candidate.created_at_ms = midnight;
    auto v = assemble(s);
    REQUIRE_THAT(v.values[position_of("hour_of_day_sin")], WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(v.values[position_of("hour_of_day_cos")], WithinAbs(1.0, 1e-6));

    s.candidate.created_at_ms = midnight + 6 * 3'600'000;  // 06:00 UTC
    v = assemble(s);
    REQUIRE_THAT(v.values[position_of("hour_of_day_sin")], WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(v.values[position_of("hour_of_day_cos")], WithinAbs(0.0, 1e-6));
}

TEST_CASE("the JSON contract and the LightGBM names mirror the array exactly") {
    const auto json = nlohmann::json::parse(feature_contract_json());
    REQUIRE(json.size() == kFeatureCount);
    const auto names = lgbm_feature_names();
    REQUIRE(names.size() == kFeatureCount);
    for (size_t i = 0; i < kFeatureCount; ++i) {
        REQUIRE(json[i]["position"].get<size_t>() == i);
        REQUIRE(json[i]["name"].get<std::string>() == std::string(kFeatures[i].name));
        REQUIRE(json[i]["availability"].get<std::string>() == "AT_SIGNAL_TIME");
        REQUIRE(names[i] == "sq_" + std::string(kFeatures[i].name));
    }
}

TEST_CASE("economics are ABSENT when no model estimated them, never zero") {
    // AltStrategy stopped producing eight of these fields, and protobuf returns
    // 0 for a field nobody set. The extractor marked all eight PRESENT, so the
    // trainer saw eight constant zeros it believed were measurements -- the
    // runtime diagnostic reported exactly "constant": 8 of 110 declared, every
    // cycle, and nothing acted on it.
    auto s = full_snapshot();
    s.candidate.edge_estimated = false;      // no active AltML model
    s.candidate.p_target_before_stop = std::nullopt;  // AltStrategy writes NULL
    s.candidate.expected_net_return = 0.0;
    s.candidate.ml_abstain = true;
    s.candidate.ml_conviction = 0.0;

    const auto v = assemble(s);
    REQUIRE(v.values.size() == kFeatureCount);

    const auto idx = [](std::string_view name) -> size_t {
        for (size_t i = 0; i < kFeatures.size(); ++i)
            if (kFeatures[i].name == name) return i;
        FAIL("feature not declared: " << name);
        return 0;
    };
    const auto present = [](const auto& vec, size_t i) {
        return (vec.validity[i / 8] & (1u << (i % 8))) != 0;
    };

    // The flag itself is an observation and is present.
    CHECK(present(v, idx("edge_estimated")));
    // The figures it gates are absent -- not zero.
    CHECK_FALSE(present(v, idx("p_target_before_stop")));
    CHECK_FALSE(present(v, idx("expected_net_return")));
    // An abstaining model has no conviction to report.
    CHECK_FALSE(present(v, idx("ml_conviction")));

    // With a real estimate they come back.
    s.candidate.edge_estimated = true;
    s.candidate.p_target_before_stop = 0.55;
    s.candidate.expected_net_return = 0.004;
    s.candidate.ml_abstain = false;
    s.candidate.ml_conviction = 0.42;
    const auto v2 = assemble(s);
    CHECK(present(v2, idx("p_target_before_stop")));
    CHECK(v2.values[idx("p_target_before_stop")] == Catch::Approx(0.55));
    CHECK(v2.values[idx("ml_conviction")] == Catch::Approx(0.42));
}

TEST_CASE("no field removed from the AltStrategy contract is still declared") {
    // Guards the contract against silently re-acquiring a member with no
    // producer: each of these was removed from altstrategy.v1 and its tag
    // reserved.
    for (std::string_view gone : {"p_entry_reached", "p_stop_before_target", "p_timeout",
                                  "p_ambiguous", "expected_gross_return", "expected_utility",
                                  "ml_uncertainty", "ml_ood"}) {
        INFO("removed upstream field: " << gone);
        bool declared = false;
        for (const auto& f : kFeatures) if (f.name == gone) declared = true;
        CHECK_FALSE(declared);
    }
}

TEST_CASE("higher-timeframe context is used, and absent when no grid had closed") {
    // §43: AltStrategy carries higher grids (27/27 contexts on VPS1 carry the
    // 30m one) and this stage did not read them at all -- `context_features`
    // was never parsed, so "does the higher frame agree with this signal" could
    // not be asked. Whether it MATTERS is a separate question, measured by
    // `ablation --arm no_htf`.
    const auto idx = [](std::string_view name) -> size_t {
        for (size_t i = 0; i < kFeatures.size(); ++i)
            if (kFeatures[i].name == name) return i;
        FAIL("feature not declared: " << name);
        return 0;
    };

    auto s = full_snapshot();
    s.candidate.side = 1;                    // LONG
    s.context.htf_minutes = 30;
    s.context.htf_features = {{"adx_14", 24.0}, {"rsi_14", 57.0},
                              {"ema_20", 101.2}, {"ema_50", 100.4}};   // higher frame UP
    const auto up = assemble(s);
    CHECK(up.observed(idx("htf_minutes")));
    CHECK(up.values[idx("htf_minutes")] == Catch::Approx(30.0));
    CHECK(up.values[idx("htf_adx_14")] == Catch::Approx(24.0));
    // Higher frame up, signal long -> agreement.
    CHECK(up.values[idx("htf_trend_agrees_with_side")] == Catch::Approx(1.0));

    // Same higher frame, opposite side -> disagreement.
    s.candidate.side = 2;                    // SHORT
    const auto down = assemble(s);
    CHECK(down.values[idx("htf_trend_agrees_with_side")] == Catch::Approx(-1.0));

    // No higher grid had closed at T0: every member is ABSENT, not zero. This
    // is the normal state for a while after a restart, and a zero here would
    // read as "the higher frame is flat", which is a different claim.
    s.context.htf_minutes = 0;
    s.context.htf_features.clear();
    const auto none = assemble(s);
    for (const char* f : {"htf_minutes", "htf_adx_14", "htf_rsi_14",
                          "htf_ema_20_50_spread_bps", "htf_trend_agrees_with_side"}) {
        INFO("feature: " << f);
        CHECK_FALSE(none.observed(idx(f)));
    }
}
