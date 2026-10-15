// Wire-contract guard: the field numbers of the altstrategy.v1 output contract
// and of the vendored altml.v1 / alttech.v1 inputs are frozen. A renumber here
// would silently break byte-compatibility with the upstream services (the
// vendored copies MUST match upstream) or with already-shipped consumers.

#include "altstrategy/engine_io/wire_codec.hpp"
#include "decision.pb.h"
#include "altml_forecast.pb.h"
#include "alttech_analytics.pb.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
int num(const google::protobuf::Descriptor* d, const char* name) {
    const auto* f = d->FindFieldByName(name);
    return f ? f->number() : -1;
}
}

TEST_CASE("altstrategy.v1 TradeSignalProposal field numbers are frozen") {
    const auto* d = altstrategy::v1::TradeSignalProposal::descriptor();
    CHECK(num(d, "signal_proposal_id") == 1);
    CHECK(num(d, "candidate_id") == 2);
    CHECK(num(d, "side") == 9);
    CHECK(num(d, "proposal_grade") == 10);
    CHECK(num(d, "stop_loss_price") == 23);
    CHECK(num(d, "take_profit_price") == 27);
    CHECK(num(d, "max_holding_time_ms") == 33);
    CHECK(num(d, "recommended_for_execution") == 54);
}

TEST_CASE("altstrategy.v1 envelope payload tags are frozen") {
    const auto* d = altstrategy::v1::Envelope::descriptor();
    CHECK(num(d, "decision_context") == 20);
    CHECK(num(d, "strategy_candidate") == 21);
    CHECK(num(d, "trade_signal_proposal") == 24);
    CHECK(num(d, "market_outcome") == 25);
    CHECK(num(d, "execution_feedback") == 33);
}

TEST_CASE("vendored altml.v1 field numbers match upstream") {
    // Pins the ONLY altml.v1 messages that have a producer. The former version
    // of this test also pinned MarketForecast/CandidateEvaluation tags, which
    // made a contract nothing ever sent look verified.
    const auto* p = altml::v1::PredictionV1::descriptor();
    CHECK(num(p, "prediction_id") == 1);
    CHECK(num(p, "timeframe") == 2);
    CHECK(num(p, "bar_open_time_ms") == 3);
    CHECK(num(p, "probability_up") == 8);
    CHECK(num(p, "expected_return") == 10);
    CHECK(num(p, "probability_target_before_stop") == 11);
    CHECK(num(p, "confidence") == 15);
    CHECK(num(p, "quality_status") == 16);
    CHECK(num(p, "prediction_schema_version") == 19);
    CHECK(num(p, "target_venue") == 20);
    CHECK(num(p, "abstain") == 21);
    const auto* e = altml::v1::PredictionEnvelope::descriptor();
    CHECK(num(e, "exchange") == 4);
    CHECK(num(e, "instrument_id") == 5);
    CHECK(num(e, "prediction") == 31);
}

TEST_CASE("vendored alttech.v1 field numbers match upstream") {
    const auto* cv = alttech::v1::CrossVenueFeatureVector::descriptor();
    CHECK(num(cv, "canonical_pair_id") == 2);
    CHECK(num(cv, "values") == 9);
    CHECK(num(cv, "quality_status") == 12);
    const auto* env = alttech::v1::Envelope::descriptor();
    CHECK(num(env, "cross_venue_feature_vector") == 61);
    CHECK(num(env, "global_market_regime") == 68);
    CHECK(num(env, "cross_venue_quality") == 69);
}

// ------------------------------------------- AltML prediction contract

// The vendored AltML contract must keep the producer's field numbers. This
// service and AltML both declare a package called `altml.v1`, and for a long
// time they declared DIFFERENT messages under it -- AltStrategy expected a
// MarketForecast with quantile sub-messages that AltML has never produced. The
// mismatch could not fail loudly: a parse of the wrong type simply returns
// nothing, so the ML path stayed silently empty. These pin the wire.
TEST_CASE("the vendored prediction envelope keeps AltML's field numbers") {
    const auto* d = altml::v1::PredictionEnvelope::descriptor();
    struct F { const char* name; int number; };
    for (const auto& f : {F{"schema_version", 1}, F{"message_id", 2}, F{"event_type", 3},
                          F{"exchange", 4}, F{"instrument_id", 5}, F{"exchange_symbol", 6},
                          F{"exchange_timestamp_ms", 7}, F{"produced_at_ms", 8},
                          F{"trace_id", 9}, F{"source_service", 10}, F{"prediction", 31}}) {
        const auto* fd = d->FindFieldByName(f.name);
        INFO("field " << f.name);
        REQUIRE(fd != nullptr);
        CHECK(fd->number() == f.number);
    }
}

TEST_CASE("the vendored prediction keeps AltML's field numbers") {
    const auto* d = altml::v1::PredictionV1::descriptor();
    struct F { const char* name; int number; };
    for (const auto& f : {F{"prediction_id", 1}, F{"timeframe", 2}, F{"bar_open_time_ms", 3},
                          F{"model_id", 5}, F{"probability_up", 8}, F{"probability_down", 9},
                          F{"expected_return", 10}, F{"expected_mfe", 12}, F{"expected_mae", 13},
                          F{"confidence", 15}, F{"quality_status", 16}, F{"computed_at_ms", 17},
                          F{"prediction_schema_version", 19}, F{"target_venue", 20},
                          F{"abstain", 21}, F{"horizon_minutes", 22},
                          F{"probability_long", 24}, F{"probability_neutral", 25},
                          F{"probability_short", 26}, F{"expected_net_return_long_bps", 27},
                          F{"expected_net_return_short_bps", 28}, F{"round_trip_cost_bps", 29}}) {
        const auto* fd = d->FindFieldByName(f.name);
        INFO("field " << f.name);
        REQUIRE(fd != nullptr);
        CHECK(fd->number() == f.number);
    }
}

TEST_CASE("a real econ prediction round-trips into the decision context view") {
    altml::v1::PredictionEnvelope env;
    env.set_schema_version(1);
    env.set_instrument_id(77);
    auto* p = env.mutable_prediction();
    p->set_prediction_id("pred-1");
    p->set_timeframe(altml::v1::TF_1M);
    p->set_bar_open_time_ms(1787000000000LL);
    p->set_bar_revision(2);
    p->set_model_id("m-1");
    p->set_model_version(3);
    p->set_probability_up(0.40);
    p->set_probability_down(0.15);
    p->set_expected_return(0.0021);
    p->set_expected_mfe(0.007);
    p->set_expected_mae(-0.003);
    p->set_confidence(0.25);
    p->set_quality_status("VALID");
    p->set_computed_at_ms(1787000060000LL);
    p->set_prediction_schema_version(4);
    p->set_target_venue(altml::v1::EXCHANGE_BITGET);
    p->set_abstain(false);
    p->set_horizon_minutes(60);
    // schema 4: market-prediction semantics
    p->set_probability_calibrated(true);
    p->set_calibration_version("temperature-1");
    p->set_label_definition("econ-class:cost-v2");
    p->set_expected_up_excursion_bps(55.0);
    p->set_expected_down_excursion_bps(48.0);
    p->set_magnitude_source("regression");
    p->set_model_role("ACTIVE");
    p->set_objective("multiclass:econ_class_1h");
    p->set_probability_long(0.40);
    p->set_probability_neutral(0.45);
    p->set_probability_short(0.15);
    p->set_expected_net_return_long_bps(5.0);
    p->set_expected_net_return_short_bps(-37.0);
    p->set_round_trip_cost_bps(16.0);
    p->set_calibrator("temperature-1");
    p->set_recipe_version("recipe-1");
    p->set_aligned_missing(1);

    const auto parsed = altstrategy::engine_io::parse_altml_forecast(env.SerializeAsString());
    REQUIRE(parsed.has_value());
    CHECK(parsed->canonical_pair_id == 77);
    const auto& v = parsed->view;
    CHECK(v.present);
    CHECK(v.econ);
    CHECK(v.horizon_minutes == 60);
    CHECK(v.p_long == Catch::Approx(0.40));
    CHECK(v.p_neutral == Catch::Approx(0.45));
    CHECK(v.p_short == Catch::Approx(0.15));
    CHECK(v.expected_net_long_bps == Catch::Approx(5.0));
    CHECK(v.expected_net_short_bps == Catch::Approx(-37.0));
    CHECK(v.round_trip_cost_bps == Catch::Approx(16.0));
    CHECK(v.aligned_missing == 1);
    CHECK(v.model_id == "m-1");
    CHECK(v.prediction_schema_version == 4);
    CHECK(v.bar_open_time_ms == 1787000000000LL);
    CHECK(v.probability_calibrated);
    CHECK(v.label_definition == "econ-class:cost-v2");
    CHECK(v.has_excursions);
    CHECK(v.expected_up_excursion_bps == Catch::Approx(55.0));
    CHECK(v.expected_down_excursion_bps == Catch::Approx(48.0));
    CHECK(v.magnitude_source == "regression");
    CHECK(v.model_role == "ACTIVE");
    // The legacy pair keeps AltML's unit (a return fraction) under its own name.
    CHECK(v.expected_mfe_long_fraction == Catch::Approx(0.007));
    CHECK(v.expected_mae_long_fraction == Catch::Approx(-0.003));
    // No per-side bps were sent: they are absent, not 0.
    CHECK_FALSE(v.has_side_bps);

    // Schema 3 carries no calibration verdict: read as uncalibrated, and the
    // excursions come from the long side's favourable / adverse fields --
    // only when all four were actually on the wire.
    p->set_prediction_schema_version(3);
    const auto s3 = altstrategy::engine_io::parse_altml_forecast(env.SerializeAsString());
    REQUIRE(s3.has_value());
    CHECK_FALSE(s3->view.probability_calibrated);
    CHECK_FALSE(s3->view.has_excursions);
    CHECK_FALSE(s3->view.has_side_bps);
    p->set_favorable_long_bps(70.0);
    p->set_adverse_long_bps(-30.0);
    p->set_favorable_short_bps(30.0);
    const auto s3_partial = altstrategy::engine_io::parse_altml_forecast(env.SerializeAsString());
    REQUIRE(s3_partial.has_value());
    CHECK_FALSE(s3_partial->view.has_side_bps);   // adverse_short missing
    p->set_adverse_short_bps(-70.0);
    const auto s3_full = altstrategy::engine_io::parse_altml_forecast(env.SerializeAsString());
    REQUIRE(s3_full.has_value());
    CHECK(s3_full->view.has_side_bps);
    CHECK(s3_full->view.favorable_short_bps == Catch::Approx(30.0));
    CHECK(s3_full->view.adverse_short_bps == Catch::Approx(-70.0));
    p->set_prediction_schema_version(4);

    // A schema-2 message missing ANY of the econ fields is not an econ
    // estimate: proto3 would have read the absent net return as 0.0 bps.
    p->clear_expected_net_return_short_bps();
    const auto partial = altstrategy::engine_io::parse_altml_forecast(env.SerializeAsString());
    REQUIRE(partial.has_value());
    CHECK_FALSE(partial->view.econ);
}

TEST_CASE("a prediction from another schema version or another venue is refused") {
    // Field meanings are only stable within a schema version, and a prediction
    // ABOUT an information venue is not evidence about the venue this engine
    // trades. Schema 1 (the retired binary direction model) is no longer read:
    // its probability is not the econ estimate the policy decides on.
    const auto make = [](int32_t schema, altml::v1::Exchange venue) {
        altml::v1::PredictionEnvelope env;
        env.set_schema_version(1);
        env.set_instrument_id(77);
        auto* p = env.mutable_prediction();
        p->set_prediction_id("pred-x");
        p->set_timeframe(altml::v1::TF_1M);
        p->set_bar_open_time_ms(1787000000000LL);
        p->set_probability_up(0.61);
        p->set_probability_down(0.39);
        p->set_prediction_schema_version(schema);
        p->set_target_venue(venue);
        return env.SerializeAsString();
    };

    // 4 is current; 3 is still read while the producer is upgraded; 2, 1 and
    // a future 5 are refused.
    CHECK(altstrategy::engine_io::parse_altml_forecast(
              make(4, altml::v1::EXCHANGE_BITGET)).has_value());
    CHECK(altstrategy::engine_io::parse_altml_forecast(
              make(3, altml::v1::EXCHANGE_BITGET)).has_value());
    CHECK_FALSE(altstrategy::engine_io::parse_altml_forecast(
                    make(2, altml::v1::EXCHANGE_BITGET)).has_value());
    CHECK_FALSE(altstrategy::engine_io::parse_altml_forecast(
                    make(1, altml::v1::EXCHANGE_BITGET)).has_value());
    CHECK_FALSE(altstrategy::engine_io::parse_altml_forecast(
                    make(5, altml::v1::EXCHANGE_BITGET)).has_value());
    CHECK_FALSE(altstrategy::engine_io::parse_altml_forecast(
                    make(4, altml::v1::EXCHANGE_BINANCE)).has_value());
    CHECK_FALSE(altstrategy::engine_io::parse_altml_forecast(
                    make(4, altml::v1::EXCHANGE_UNSPECIFIED)).has_value());
}

TEST_CASE("an envelope carrying no prediction is refused") {
    altml::v1::PredictionEnvelope env;
    env.set_schema_version(1);
    CHECK_FALSE(altstrategy::engine_io::parse_altml_forecast(env.SerializeAsString()).has_value());
}
