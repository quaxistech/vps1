// The canonicaliser: the projection from AltSignalQuality's FINAL signal onto
// the self-contained contract an external consumer receives.
//
// Two properties are load-bearing and each has its own section below:
//
//   * NOTHING IS INVENTED. Wherever an upstream encodes "absent" as 0.0 or as a
//     protobuf default, the projection must say "absent" rather than forwarding
//     a number a consumer could act on.
//   * THE BYTES ARE STABLE. The integrity hash is taken over the serialized
//     payload, so the same inputs must always produce the same bytes.

#include "altsignalgateway/core/codec.hpp"
#include "altsignalgateway/core/hash.hpp"
#include "altsignalgateway/domain/canonical.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace altsignalgateway::domain;
namespace pb = altsignalgateway::v1;
namespace as = altstrategy::v1;
namespace sq = altsignalquality::v1;

namespace {

constexpr int64_t kT0 = 1'787'676'374'679;

sq::FinalTradeSignal signal_fixture() {
    sq::FinalTradeSignal sig;
    sig.set_final_signal_id("sqfin_deadbeefdeadbeefdeadbeefdeadbeef");
    sig.set_published_at_ms(kT0 + 400);
    sig.set_producer_version("1.0.0");

    auto* p = sig.mutable_proposal();
    p->set_signal_proposal_id("sig_9a1e4d57883b376b582955c0");
    p->set_candidate_id("cand_1");
    p->set_ranking_id("rank_1");
    p->set_canonical_pair_id(4);
    p->set_strategy_id("volume_weighted_momentum_burst");
    p->set_strategy_version("1.0.0");
    p->set_parameter_set_id("ps_1");
    p->set_side(as::SIDE_LONG);
    p->set_proposal_grade(as::GRADE_NORMAL);
    p->set_entry_type(as::ENTRY_MARKET);
    p->set_entry_price(1.4733);
    p->set_stop_loss_price(1.4564);
    p->set_take_profit_price(1.4878);
    p->set_created_at_ms(kT0);
    p->set_valid_until_ms(kT0 + 1'350'000);
    p->set_max_holding_time_ms(28'800'000);
    p->set_time_stop_at_ms(kT0 + 28'800'000);
    p->set_holding_class(as::HOLDING_INTRADAY);
    p->set_recommended_for_execution(true);
    p->set_context_revision(1'787'675'400'000);
    p->set_feature_manifest_hash("934cb08abe0c342c");
    p->set_expected_rr(0.858);
    p->set_state(as::STATE_PROPOSED);
    p->set_quality(as::CONTEXT_STRICT_4_OF_4);

    auto* q = sig.mutable_quality();
    q->set_evaluation_id("sqev_abc");
    q->set_decision(sq::DECISION_ACCEPTED);
    q->set_basis(sq::BASIS_MODEL);
    q->set_has_quality_score(true);
    q->set_quality_score(0.61);
    q->set_raw_score(0.72);
    q->set_decision_threshold(0.55);
    q->set_model_id("20260823T030000Z-lightgbm-a1b2c3d4");
    q->set_model_version(3);
    q->set_context_id("ctx_795d0a88a77a684969d6f689");
    q->set_context_revision(1'787'675'400'000);
    q->set_signal_created_at_ms(kT0);
    q->set_evaluated_at_ms(kT0 + 200);
    return sig;
}

ContextSnapshot context_fixture() {
    ContextSnapshot ctx;
    ctx.present = true;
    ctx.context_id = "ctx_795d0a88a77a684969d6f689";
    ctx.context_revision = 1'787'675'400'000;
    ctx.event_time_ms = 1'787'675'400'000;
    ctx.available_at_ms = kT0;
    ctx.expires_at_ms = kT0 + 300'000;
    ctx.global_universe_status = "ELIGIBLE_ALL_VENUES";
    ctx.data_quality = "STRICT_4_OF_4";
    ctx.venue_mask = 15;
    ctx.mark_price = 1.4730;
    ctx.spread_bps = 10.0;
    ctx.fees_bps = 4.0;
    // Deliberately left unset: AltStrategy writes NULL when it has no reading.
    ctx.liquidity_rank = std::nullopt;
    // Out of order on purpose — the canonicaliser must sort them.
    ctx.venue_features = {{"rsi_14", 47.8}, {"adx_14", 17.3}, {"atr_14", 1.06}};
    ctx.microstructure = {{"cvd", -84736.4}, {"trade_intensity", 0.0}};
    ctx.venue_regime.regime = "RANGE";
    ctx.venue_regime.confidence = 0.5;
    ctx.global_regime.regime = "RANGE";
    return ctx;
}

InstrumentResolution instrument_fixture() {
    InstrumentResolution inst;
    inst.present = true;
    inst.exchange = "bitget";
    inst.base_asset = "XRP";
    inst.quote_asset = "USDT";
    inst.market_type = "usdt_perp";
    inst.exchange_symbol = "XRPUSDT";
    inst.exchange_instrument_id = 13;
    inst.instrument_status = "active";
    inst.whitelisted = true;
    inst.spec_present = true;
    inst.spec_version = 1;
    inst.spec_valid_from_ms = kT0 - 86'400'000;
    inst.contract_type = "perpetual";
    inst.settle_asset = "USDT";
    inst.tick_size = "0.0001";
    inst.quantity_step = "1";
    inst.min_notional = "5";
    inst.min_order_quantity = "1";
    inst.contract_value = "1";
    inst.price_precision = 4;
    inst.quantity_precision = 0;
    inst.max_leverage = 75.0;
    inst.resolved_at_ms = kT0 + 600;
    return inst;
}

IngestMetadata meta_fixture() {
    IngestMetadata m;
    m.source_subject = "signalquality.final.pair-4.v1";
    m.source_stream = "ALTSIGNALQUALITY_SIGNALS";
    m.source_stream_sequence = 4242;
    m.received_at_ms = kT0 + 500;
    m.journaled_at_ms = kT0 + 600;
    m.message_id = "asgw_00112233445566778899aabbccddeeff";
    m.gateway_version = "1.0.0";
    return m;
}

} // namespace

// AltTech's macro block as of T0, as the store would hand it over: two usable
// positions, one withheld, sorted by name.
MacroSnapshot macro_fixture() {
    MacroSnapshot m;
    m.present = true;
    m.feature_set_name = "macro-core";
    m.feature_set_version = 1;
    m.schema_hash = "58bc7b7239b3";
    m.method_version = "mc-1.2.0";
    m.bar_open_time_ms = 1788501600000LL;
    m.computed_at_ms = 1788501661000LL;
    m.age_ms = 36363;
    m.status = "INCOMPLETE_INPUT";
    m.feature_count = 3;
    m.valid_count = 2;
    m.values = {{"us10y_level", 4.21}, {"vix_level", 17.9}};
    m.unavailable = {"cnh_ret_5m"};
    return m;
}

TEST_CASE("the macro block is carried as of T0 with its own freshness, never as zeros") {
    // schema 2: the block is one vector per minute for the whole system, and a
    // consumer reads what was usable at T0 and what was not -- by name, not by
    // a 0.0 that would look like a reading of nothing happening.
    const auto out = build_payload(signal_fixture(), "x", context_fixture(), instrument_fixture(),
                                   macro_fixture(), meta_fixture());
    REQUIRE(out.macro().present());
    CHECK(out.macro().feature_set_name() == "macro-core");
    CHECK(out.macro().method_version() == "mc-1.2.0");
    CHECK(out.macro().bar_open_time_ms() == 1788501600000LL);
    CHECK(out.macro().age_ms() == 36363);
    CHECK(out.macro().status() == "INCOMPLETE_INPUT");
    REQUIRE(out.macro().values_size() == 2);
    CHECK(out.macro().values(0).name() == "us10y_level");
    CHECK(out.macro().values(1).name() == "vix_level");
    REQUIRE(out.macro().unavailable_size() == 1);
    CHECK(out.macro().unavailable(0) == "cnh_ret_5m");
    CHECK(out.macro().source_service() == "alttech");

    // Absent is stated with a reason, and nothing else is written.
    MacroSnapshot none;
    none.present = false;
    none.absence_reason = "MACRO_READ_FAILED";
    const auto o2 = build_payload(signal_fixture(), "x", context_fixture(), instrument_fixture(),
                                  none, meta_fixture());
    CHECK_FALSE(o2.macro().present());
    CHECK(o2.macro().absence_reason() == "MACRO_READ_FAILED");
    CHECK(o2.macro().values_size() == 0);

    // The history-page projection reflects the block's presence and copies the
    // decision scalars without touching them.
    const auto summary = summarize_payload(out, "ACKED", 1788502900000LL);
    CHECK(summary.macro_present());
    CHECK(summary.symbol() == out.trade_plan().symbol());
    CHECK(summary.entry_price() == out.trade_plan().entry_price());
    CHECK(summary.expected_rr() == out.trade_plan().expected_rr());
    CHECK(summary.delivery_state() == "ACKED");
    CHECK(summary.last_ack_at_ms() == 1788502900000LL);
}

TEST_CASE("the trade plan is copied, not recomputed") {
    const auto sig = signal_fixture();
    const auto out = build_payload(sig, "SOURCEBYTES", context_fixture(), instrument_fixture(), macro_fixture(),
                                   meta_fixture());
    const auto& t = out.trade_plan();
    CHECK(t.side() == as::SIDE_LONG);
    CHECK(t.entry_type() == as::ENTRY_MARKET);
    CHECK(t.entry_price() == sig.proposal().entry_price());
    CHECK(t.stop_loss_price() == sig.proposal().stop_loss_price());
    CHECK(t.take_profit_price() == sig.proposal().take_profit_price());
    CHECK(t.valid_until_ms() == sig.proposal().valid_until_ms());
    CHECK(t.max_holding_time_ms() == sig.proposal().max_holding_time_ms());
    CHECK(t.strategy_id() == sig.proposal().strategy_id());
}

TEST_CASE("the executable symbol comes from the registry, not from the wire") {
    // The upstream carries an internal token; on this deployment it is
    // literally "pair-4", which no venue has ever heard of.
    const auto out = build_payload(signal_fixture(), "x", context_fixture(),
                                   instrument_fixture(), macro_fixture(), meta_fixture());
    CHECK(out.trade_plan().symbol() == "XRPUSDT");
    CHECK(out.trade_plan().exchange() == "bitget");
    CHECK(out.trade_plan().base_asset() == "XRP");
    CHECK(out.trade_plan().canonical_pair_id() == 4);
}

TEST_CASE("an absent optional price is absent, never zero") {
    auto sig = signal_fixture();
    // AltStrategy writes exactly 0.0 for "no range", "no trigger", "no second
    // target". Forwarding those as prices would offer a consumer a price of
    // zero and let it act on one.
    sig.mutable_proposal()->set_entry_lower(0);
    sig.mutable_proposal()->set_entry_upper(0);
    sig.mutable_proposal()->set_trigger_price(0);
    sig.mutable_proposal()->set_optional_secondary_target(0);

    const auto out = build_payload(sig, "x", context_fixture(), instrument_fixture(), macro_fixture(),
                                   meta_fixture());
    const auto& t = out.trade_plan();
    CHECK_FALSE(t.has_entry_range());
    CHECK_FALSE(t.has_trigger_price());
    CHECK_FALSE(t.has_secondary_target());
    CHECK(t.entry_lower() == 0.0);
    CHECK(t.trigger_price() == 0.0);

    SECTION("a populated one is carried and flagged") {
        sig.mutable_proposal()->set_entry_lower(1.470);
        sig.mutable_proposal()->set_entry_upper(1.476);
        sig.mutable_proposal()->set_trigger_price(1.4750);
        sig.mutable_proposal()->set_optional_secondary_target(1.4950);
        const auto o2 = build_payload(sig, "x", context_fixture(), instrument_fixture(), macro_fixture(),
                                      meta_fixture());
        CHECK(o2.trade_plan().has_entry_range());
        CHECK(o2.trade_plan().entry_lower() == 1.470);
        CHECK(o2.trade_plan().has_trigger_price());
        CHECK(o2.trade_plan().has_secondary_target());
        CHECK(o2.trade_plan().secondary_target_price() == 1.4950);
    }
}

TEST_CASE("AltML is reported absent when AltML did not run") {
    // This is the deployed state today: no model_versions, ml_abstain=true,
    // ml_conviction=0 — protobuf defaults, not measurements. Forwarding them
    // would tell a consumer a confident model ran when none did.
    auto sig = signal_fixture();
    sig.mutable_proposal()->set_ml_abstain(true);
    sig.mutable_proposal()->set_ml_conviction(0);

    const auto out = build_payload(sig, "x", context_fixture(), instrument_fixture(), macro_fixture(),
                                   meta_fixture());
    CHECK_FALSE(out.altml().present());
    CHECK(out.altml().absence_reason() == "NO_MODEL_VERSIONS_ON_PROPOSAL");
    CHECK(out.altml().model_versions_size() == 0);
    CHECK(out.altml().conviction() == 0.0);
    CHECK_FALSE(out.altml().abstain());

    SECTION("and present once AltML actually contributes") {
        sig.mutable_proposal()->add_model_versions("altml-forecast:7");
        sig.mutable_proposal()->set_ml_conviction(0.31);
        sig.mutable_proposal()->set_ml_abstain(false);
        const auto o2 = build_payload(sig, "x", context_fixture(), instrument_fixture(), macro_fixture(),
                                      meta_fixture());
        CHECK(o2.altml().present());
        CHECK(o2.altml().conviction() == 0.31);
        CHECK(o2.altml().model_versions(0) == "altml-forecast:7");
    }
}

// Regression: the vendored altstrategy contract was ten days stale, and the
// fields it still declared had been RESERVED upstream for never having been
// computed. Reading them yielded protobuf zeros, and the gateway shipped those
// zeros as measurements -- `ood_score: 0` reading as a measured all-clear on a
// check that never ran, four zero probabilities standing beside the one real
// one. What the producer does compute (`ml_conviction`, and `edge_estimated` as
// the authority on whether economics exist at all) was not read at all, because
// the stale copy did not declare it.
TEST_CASE("only what the producer actually computes reaches the payload",
          "[canonical][regression]") {
    SECTION("no economics without edge_estimated, whatever the values say") {
        auto sig = signal_fixture();
        sig.mutable_proposal()->set_edge_estimated(false);
        sig.mutable_proposal()->set_p_target_before_stop(0.61);
        sig.mutable_proposal()->set_expected_net_return(0.0042);
        const auto out = build_payload(sig, "x", context_fixture(), instrument_fixture(), macro_fixture(),
                                       meta_fixture());
        CHECK_FALSE(out.strategy().has_outcome_probabilities());
        CHECK_FALSE(out.strategy().has_expected_returns());
        CHECK(out.strategy().p_target_before_stop() == 0.0);
        CHECK(out.strategy().expected_net_return() == 0.0);
    }

    SECTION("economics are carried once the producer says it estimated them") {
        auto sig = signal_fixture();
        sig.mutable_proposal()->set_edge_estimated(true);
        sig.mutable_proposal()->set_p_target_before_stop(0.61);
        sig.mutable_proposal()->set_expected_net_return(0.0042);
        const auto out = build_payload(sig, "x", context_fixture(), instrument_fixture(), macro_fixture(),
                                       meta_fixture());
        CHECK(out.strategy().has_outcome_probabilities());
        CHECK(out.strategy().p_target_before_stop() == 0.61);
        CHECK(out.strategy().has_expected_returns());
        CHECK(out.strategy().expected_net_return() == 0.0042);
    }

    SECTION("an estimated net return of exactly zero is still an estimate") {
        // The old gate probed the VALUES for nonzero-ness, so a forecast that
        // genuinely expected zero was dropped as if nothing had been computed.
        auto sig = signal_fixture();
        sig.mutable_proposal()->set_edge_estimated(true);
        sig.mutable_proposal()->set_expected_net_return(0.0);
        sig.mutable_proposal()->set_p_target_before_stop(0.5);
        const auto out = build_payload(sig, "x", context_fixture(), instrument_fixture(), macro_fixture(),
                                       meta_fixture());
        CHECK(out.strategy().has_expected_returns());
        CHECK(out.strategy().expected_net_return() == 0.0);
    }
}

TEST_CASE("an all-zero probability vector is not a forecast") {
    auto sig = signal_fixture();
    const auto out = build_payload(sig, "x", context_fixture(), instrument_fixture(), macro_fixture(),
                                   meta_fixture());
    CHECK_FALSE(out.strategy().has_outcome_probabilities());
    CHECK_FALSE(out.strategy().has_expected_returns());

    // Values alone no longer open the block: the producer's own
    // `edge_estimated` flag decides, so a plan carrying leftover numbers
    // without an estimate behind them still reports nothing.
    sig.mutable_proposal()->set_p_target_before_stop(0.43);
    sig.mutable_proposal()->set_expected_net_return(0.0031);
    const auto o2 = build_payload(sig, "x", context_fixture(), instrument_fixture(), macro_fixture(),
                                  meta_fixture());
    CHECK_FALSE(o2.strategy().has_outcome_probabilities());
    CHECK_FALSE(o2.strategy().has_expected_returns());

    sig.mutable_proposal()->set_edge_estimated(true);
    const auto o3 = build_payload(sig, "x", context_fixture(), instrument_fixture(), macro_fixture(),
                                  meta_fixture());
    CHECK(o3.strategy().has_outcome_probabilities());
    CHECK(o3.strategy().p_target_before_stop() == 0.43);
    CHECK(o3.strategy().has_expected_returns());
}

TEST_CASE("a missing market snapshot is stated, never filled in") {
    ContextSnapshot missing;
    missing.present = false;
    missing.absence_reason = "CONTEXT_SNAPSHOT_NOT_FOUND";
    const auto out = build_payload(signal_fixture(), "x", missing, instrument_fixture(), macro_fixture(),
                                   meta_fixture());
    CHECK_FALSE(out.market_snapshot().present());
    CHECK(out.market_snapshot().absence_reason() == "CONTEXT_SNAPSHOT_NOT_FOUND");
    CHECK(out.market_snapshot().venue_features_size() == 0);
    CHECK_FALSE(out.microstructure().present());
    CHECK_FALSE(out.regime().present());
    // The trade itself is unaffected: an absent snapshot is a missing
    // explanation, not a missing trade.
    CHECK(out.trade_plan().entry_price() == signal_fixture().proposal().entry_price());
}

TEST_CASE("an unmeasured venue scalar is absent") {
    auto ctx = context_fixture();
    ctx.liquidity_rank = std::nullopt;
    ctx.top_depth_usdt = std::nullopt;
    const auto out = build_payload(signal_fixture(), "x", ctx, instrument_fixture(), macro_fixture(),
                                   meta_fixture());
    const auto& ms = out.market_snapshot();
    CHECK(ms.present());
    CHECK(ms.has_mark_price());
    CHECK(ms.mark_price() == 1.4730);
    // A 0 bps spread and an unknown spread are not the same market.
    CHECK_FALSE(ms.has_liquidity_rank());
    CHECK_FALSE(ms.has_top_depth_usdt());
    CHECK(ms.liquidity_rank() == 0.0);
}

TEST_CASE("named analytics are sorted so the bytes are canonical") {
    const auto out = build_payload(signal_fixture(), "x", context_fixture(),
                                   instrument_fixture(), macro_fixture(), meta_fixture());
    const auto& f = out.market_snapshot().venue_features();
    REQUIRE(f.size() == 3);
    CHECK(f[0].name() == "adx_14");
    CHECK(f[1].name() == "atr_14");
    CHECK(f[2].name() == "rsi_14");
}

TEST_CASE("a non-finite analytic is dropped rather than transmitted") {
    auto ctx = context_fixture();
    ctx.venue_features.push_back({"broken", std::nan("")});
    const auto out = build_payload(signal_fixture(), "x", ctx, instrument_fixture(), macro_fixture(),
                                   meta_fixture());
    for (const auto& v : out.market_snapshot().venue_features()) {
        CHECK(v.name() != "broken");
    }
}

TEST_CASE("execution constraints are exact decimals") {
    const auto out = build_payload(signal_fixture(), "x", context_fixture(),
                                   instrument_fixture(), macro_fixture(), meta_fixture());
    const auto& e = out.execution_context();
    CHECK(e.present());
    CHECK(e.exchange_symbol() == "XRPUSDT");
    // Strings, not doubles: "0.0001" as a double is not 0.0001, and rounding a
    // price to that is how an order gets rejected for an invalid tick.
    CHECK(e.tick_size() == "0.0001");
    CHECK(e.min_notional() == "5");
    CHECK(e.has_price_precision());
    CHECK(e.price_precision() == 4);
    CHECK(e.has_max_leverage());
    CHECK(e.exchange_instrument_id() == 13);
}

TEST_CASE("an unresolved instrument and a missing spec are different failures") {
    SECTION("no venue mapping at all") {
        InstrumentResolution none;
        none.present = false;
        none.absence_reason = "NO_VENUE_MAPPING_FOR_CANONICAL_PAIR";
        const auto out = build_payload(signal_fixture(), "x", context_fixture(), none,
                                       macro_fixture(), meta_fixture());
        CHECK_FALSE(out.execution_context().present());
        CHECK(out.execution_context().absence_reason() == "NO_VENUE_MAPPING_FOR_CANONICAL_PAIR");
        CHECK(out.trade_plan().symbol().empty());
    }
    SECTION("a symbol but no spec") {
        auto inst = instrument_fixture();
        inst.spec_present = false;
        inst.spec_absence_reason = "NO_INSTRUMENT_SPEC_AS_OF_SIGNAL_TIME";
        const auto out = build_payload(signal_fixture(), "x", context_fixture(), inst,
                                       macro_fixture(), meta_fixture());
        CHECK_FALSE(out.execution_context().present());
        CHECK(out.execution_context().absence_reason() == "NO_INSTRUMENT_SPEC_AS_OF_SIGNAL_TIME");
        // The symbol still resolved, and saying so is more useful than saying
        // nothing.
        CHECK(out.execution_context().exchange_symbol() == "XRPUSDT");
        CHECK(out.trade_plan().symbol() == "XRPUSDT");
        CHECK(out.execution_context().tick_size().empty());
    }
}

TEST_CASE("provenance carries the whole lineage and the source hash") {
    const std::string source = "the exact bytes AltSignalQuality published";
    const auto out = build_payload(signal_fixture(), source, context_fixture(),
                                   instrument_fixture(), macro_fixture(), meta_fixture());
    const auto& pv = out.provenance();
    CHECK(pv.signal_proposal_id() == "sig_9a1e4d57883b376b582955c0");
    CHECK(pv.final_signal_id() == "sqfin_deadbeefdeadbeefdeadbeefdeadbeef");
    CHECK(pv.context_id() == "ctx_795d0a88a77a684969d6f689");
    CHECK(pv.quality_model_id() == "20260823T030000Z-lightgbm-a1b2c3d4");
    CHECK(pv.signal_created_at_ms() == kT0);
    CHECK(pv.context_event_time_ms() == 1'787'675'400'000);
    CHECK(pv.gateway_journaled_at_ms() == kT0 + 600);
    CHECK(pv.source_stream_sequence() == 4242);
    CHECK(pv.altsignalquality_version() == "1.0.0");
    CHECK(pv.altsignalgateway_version() == "1.0.0");
    CHECK(pv.source_envelope_sha256() == altsignalgateway::sha256_hex(source));
}

TEST_CASE("the original upstream payload is embedded byte for byte") {
    const std::string source("\x00\x01\x02binary\xff", 10);
    const auto out = build_payload(signal_fixture(), source, context_fixture(),
                                   instrument_fixture(), macro_fixture(), meta_fixture());
    CHECK(out.source_envelope() == source);
    CHECK(out.provenance().source_envelope_sha256() == altsignalgateway::sha256_hex(source));
}

TEST_CASE("point-in-time: nothing that appeared after T0 leaks into the snapshot") {
    // The snapshot's own timestamps are the context's, not the moment the
    // Gateway read it. A consumer reading event_time_ms is reading the market
    // the decision was taken on.
    const auto out = build_payload(signal_fixture(), "x", context_fixture(),
                                   instrument_fixture(), macro_fixture(), meta_fixture());
    CHECK(out.market_snapshot().event_time_ms() == 1'787'675'400'000);
    CHECK(out.market_snapshot().event_time_ms() <= out.provenance().signal_created_at_ms());
    CHECK(out.market_snapshot().available_at_ms() <= out.provenance().signal_created_at_ms());
    // The spec is the version in force at T0, not the newest one.
    CHECK(out.execution_context().spec_valid_from_ms() <= out.provenance().signal_created_at_ms());
}

TEST_CASE("canonical serialization is deterministic") {
    const auto a = build_payload(signal_fixture(), "src", context_fixture(),
                                 instrument_fixture(), macro_fixture(), meta_fixture());
    const auto b = build_payload(signal_fixture(), "src", context_fixture(),
                                 instrument_fixture(), macro_fixture(), meta_fixture());
    auto sa = altsignalgateway::messaging::canonical_serialize(a);
    auto sb = altsignalgateway::messaging::canonical_serialize(b);
    REQUIRE(sa.has_value());
    REQUIRE(sb.has_value());
    CHECK(*sa == *sb);

    // And it survives a round trip: this is exactly the check the delivery path
    // performs before every transmission.
    pb::SignalPayload parsed;
    REQUIRE(parsed.ParseFromString(*sa));
    auto again = altsignalgateway::messaging::canonical_serialize(parsed);
    REQUIRE(again.has_value());
    CHECK(*again == *sa);
    CHECK(altsignalgateway::sha256_hex(*again) == altsignalgateway::sha256_hex(*sa));
}

TEST_CASE("the hash changes when the trade changes") {
    auto sig = signal_fixture();
    auto base = altsignalgateway::messaging::canonical_hash(
        build_payload(sig, "src", context_fixture(), instrument_fixture(), macro_fixture(), meta_fixture()));
    REQUIRE(base.has_value());

    sig.mutable_proposal()->set_stop_loss_price(1.4500);
    auto moved = altsignalgateway::messaging::canonical_hash(
        build_payload(sig, "src", context_fixture(), instrument_fixture(), macro_fixture(), meta_fixture()));
    REQUIRE(moved.has_value());
    CHECK(*base != *moved);
}

TEST_CASE("the envelope describes the transmission, not the trade") {
    EnvelopeSource src;
    src.message_id = "asgw_1";
    src.signal_id = "sqfin_1";
    src.payload_sha256 = std::string(64, 'a');
    src.sequence = 101;
    src.created_at_ms = kT0;
    src.expires_at_ms = kT0 + 1000;
    src.journaled_at_ms = kT0 + 100;
    src.producer_version = "1.0.0";

    const auto live = build_envelope(src, false, 1, kT0 + 200);
    CHECK(live.sequence() == 101);
    CHECK(live.signal_type() == "FINAL_TRADE_SIGNAL");
    CHECK(live.producer_service() == "altsignalgateway");
    CHECK_FALSE(live.replay());
    CHECK(live.delivery_attempt() == 1);
    CHECK(live.published_at_ms() == kT0 + 200);

    // A redelivery carries the same identity and payload hash but its own
    // transmission metadata.
    const auto replay = build_envelope(src, true, 2, kT0 + 5000);
    CHECK(replay.signal_id() == live.signal_id());
    CHECK(replay.payload_sha256() == live.payload_sha256());
    CHECK(replay.sequence() == live.sequence());
    CHECK(replay.replay());
    CHECK(replay.delivery_attempt() == 2);
    CHECK(replay.published_at_ms() != live.published_at_ms());
}

TEST_CASE("an unknown context-quality token does not become a plausible one") {
    CHECK(parse_context_quality("STRICT_4_OF_4") == as::CONTEXT_STRICT_4_OF_4);
    CHECK(parse_context_quality("DEGRADED_3_OF_4") == as::CONTEXT_DEGRADED_3_OF_4);
    CHECK(parse_context_quality("SOMETHING_NEW") == as::CONTEXT_QUALITY_UNSPECIFIED);
    CHECK(parse_context_quality("") == as::CONTEXT_QUALITY_UNSPECIFIED);
}

TEST_CASE("the quality block forwards the verdict verbatim") {
    const auto out = build_payload(signal_fixture(), "x", context_fixture(),
                                   instrument_fixture(), macro_fixture(), meta_fixture());
    const auto& g = out.signal_quality();
    CHECK(g.decision() == sq::DECISION_ACCEPTED);
    CHECK(g.basis() == sq::BASIS_MODEL);
    CHECK(g.has_quality_score());
    CHECK(g.quality_score() == 0.61);
    CHECK(g.decision_threshold() == 0.55);

    SECTION("a cold-start decision carries no score at all") {
        auto sig = signal_fixture();
        sig.mutable_quality()->set_has_quality_score(false);
        sig.mutable_quality()->set_basis(sq::BASIS_COLD_START);
        sig.mutable_quality()->set_quality_score(0.5);   // meaningless upstream
        const auto o2 = build_payload(sig, "x", context_fixture(), instrument_fixture(), macro_fixture(),
                                      meta_fixture());
        CHECK_FALSE(o2.signal_quality().has_quality_score());
        // The meaningless 0.5 is NOT forwarded: a consumer sizing on it would
        // be sizing on a protobuf default.
        CHECK(o2.signal_quality().quality_score() == 0.0);
        CHECK(o2.signal_quality().basis() == sq::BASIS_COLD_START);
    }
}

// ---------------------------------------------------------------------------
// Schema 3: the VPS1 signal's market view and setup description.
// ---------------------------------------------------------------------------

namespace {
void add_prediction(as::TradeSignalProposal* p, int32_t h, const std::string& state, bool values,
                    double up = 0.4, double flat = 0.35, double down = 0.25) {
    auto* m = p->add_market_predictions();
    m->set_horizon_minutes(h);
    m->set_state(state);
    m->set_has_values(values);
    m->set_model_id("20260926T000000Z-lightgbm-econ-" + std::to_string(h));
    m->set_model_version(1);
    m->set_model_role("ACTIVE");
    m->set_calibration_version("temperature-1");
    m->set_probability_calibrated(true);
    m->set_bar_open_time_ms(kT0 - 120'000);
    if (values) {
        m->set_p_up(up); m->set_p_flat(flat); m->set_p_down(down);
        m->set_expected_return_bps(3.5);
        m->set_expected_up_excursion_bps(42.0);
        m->set_expected_down_excursion_bps(37.0);
        m->set_cost_model_version("cost-v2");
        m->set_cost_bps(16.2);
        m->set_expected_net_long_bps(-12.7);
        m->set_expected_net_short_bps(-19.7);
    }
}
} // namespace

TEST_CASE("schema 3: every horizon of the market view is carried, in horizon order",
          "[canonical][schema3][vps1]") {
    auto sig = signal_fixture();
    auto* p = sig.mutable_proposal();
    add_prediction(p, 240, "MISSING", false);
    add_prediction(p, 15, "PRESENT", true);
    add_prediction(p, 60, "STALE", true);
    p->set_evaluation_horizon_minutes(240);
    p->set_signal_semantics("vps1-signal-1");

    const auto out = build_payload(sig, "x", context_fixture(), instrument_fixture(), macro_fixture(),
                                   meta_fixture());
    const auto& f = out.market_forecast();
    REQUIRE(f.present());
    CHECK(f.evaluation_horizon_minutes() == 240);
    CHECK(f.signal_semantics() == "vps1-signal-1");
    REQUIRE(f.horizons_size() == 3);
    CHECK(f.horizons(0).horizon_minutes() == 15);
    CHECK(f.horizons(1).horizon_minutes() == 60);
    CHECK(f.horizons(2).horizon_minutes() == 240);
    // PRESENT: the numbers, the calibration verdict and the age at T0.
    CHECK(f.horizons(0).present());
    CHECK(f.horizons(0).p_up() == 0.4);
    CHECK(f.horizons(0).expected_up_excursion_bps() == 42.0);
    CHECK(f.horizons(0).probability_calibrated());
    CHECK(f.horizons(0).age_ms() == 60'000);
    // STALE still carries numbers; the state says in what condition.
    CHECK(f.horizons(1).present());
    CHECK(f.horizons(1).state() == "STALE");
    // MISSING carries none -- absent, never zeros read as a forecast.
    CHECK_FALSE(f.horizons(2).present());
    CHECK(f.horizons(2).state() == "MISSING");
    CHECK(f.horizons(2).p_up() == 0.0);
    CHECK(out.strategy().recommended_for_execution() == true);   // copied verbatim, never decided here
}

TEST_CASE("schema 3: incoherent prediction values are refused as absent", "[canonical][schema3]") {
    auto sig = signal_fixture();
    add_prediction(sig.mutable_proposal(), 15, "PRESENT", true, 0.7, 0.7, 0.7);   // sums to 2.1
    const auto out = build_payload(sig, "x", context_fixture(), instrument_fixture(), macro_fixture(),
                                   meta_fixture());
    REQUIRE(out.market_forecast().horizons_size() == 1);
    CHECK_FALSE(out.market_forecast().horizons(0).present());
    CHECK(out.market_forecast().horizons(0).state() == "INCOHERENT_VALUES");
    CHECK(out.market_forecast().horizons(0).p_up() == 0.0);
}

TEST_CASE("schema 3: a proposal without a market view says so", "[canonical][schema3]") {
    const auto out = build_payload(signal_fixture(), "x", context_fixture(), instrument_fixture(),
                                   macro_fixture(), meta_fixture());
    CHECK_FALSE(out.market_forecast().present());
    CHECK(out.market_forecast().absence_reason() == "NO_MARKET_PREDICTIONS_ON_PROPOSAL");
}

TEST_CASE("schema 3: setup descriptors are sorted and finite", "[canonical][schema3]") {
    auto sig = signal_fixture();
    auto* p = sig.mutable_proposal();
    for (const auto& [n, v] : std::vector<std::pair<std::string, double>>{
             {"room_to_zone_atr", 2.5}, {"break_distance_atr", 0.3}, {"bad", std::nan("")}}) {
        auto* d = p->add_setup_descriptors();
        d->set_name(n);
        d->set_value(v);
    }
    const auto out = build_payload(sig, "x", context_fixture(), instrument_fixture(), macro_fixture(),
                                   meta_fixture());
    CHECK(out.setup().setup_id() == "volume_weighted_momentum_burst");
    REQUIRE(out.setup().descriptors_size() == 2);
    CHECK(out.setup().descriptors(0).name() == "break_distance_atr");
    CHECK(out.setup().descriptors(1).name() == "room_to_zone_atr");
}
