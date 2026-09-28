// The wire contract is frozen once a consumer ships. A renumbered field does not
// fail to compile and does not fail to parse — it silently decodes as a
// different field, so the numbers are asserted here rather than reviewed.

#include "signalquality.pb.h"
#include "vendor/decision.pb.h"

#include "altsignalquality/core/codec.hpp"
#include "altsignalquality/domain/policy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <google/protobuf/descriptor.h>

#include <array>
#include <string>

namespace {

const google::protobuf::Descriptor* desc(const std::string& full_name) {
    const auto* d = google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(
        full_name);
    REQUIRE(d != nullptr);
    return d;
}

void require_field(const std::string& message, const std::string& field, int number) {
    const auto* f = desc(message)->FindFieldByName(field);
    INFO(message << "." << field);
    REQUIRE(f != nullptr);
    REQUIRE(f->number() == number);
}

} // namespace

TEST_CASE("the package and schema version are what consumers were told") {
    REQUIRE(desc("altsignalquality.v1.FinalTradeSignal") != nullptr);
    REQUIRE(altsignalquality::messaging::kSchemaVersion == 1);
}

TEST_CASE("SignalQualityEvaluation field numbers are frozen") {
    const std::string m = "altsignalquality.v1.SignalQualityEvaluation";
    require_field(m, "evaluation_id", 1);
    require_field(m, "signal_proposal_id", 2);
    require_field(m, "candidate_id", 3);
    require_field(m, "canonical_pair_id", 4);
    require_field(m, "symbol", 5);
    require_field(m, "strategy_id", 6);
    require_field(m, "side", 8);
    require_field(m, "upstream_recommended", 10);
    require_field(m, "has_quality_score", 11);
    require_field(m, "quality_score", 12);
    require_field(m, "raw_score", 13);
    require_field(m, "decision_threshold", 14);
    require_field(m, "decision", 15);
    require_field(m, "basis", 16);
    require_field(m, "reason_codes", 17);
    require_field(m, "model_id", 18);
    require_field(m, "feature_schema_version", 20);
    require_field(m, "feature_schema_hash", 21);
    require_field(m, "calibration_version", 22);
    require_field(m, "signal_created_at_ms", 28);
    require_field(m, "evaluated_at_ms", 29);
}

TEST_CASE("FinalTradeSignal carries AltStrategy's proposal unchanged") {
    const std::string m = "altsignalquality.v1.FinalTradeSignal";
    require_field(m, "final_signal_id", 1);
    require_field(m, "proposal", 2);
    require_field(m, "quality", 3);
    // The proposal field must be the PRODUCER's message type, not a restatement
    // of it: the final signal adds a verdict, it never rewrites the trade.
    const auto* proposal = desc(m)->FindFieldByName("proposal");
    REQUIRE(proposal->message_type()->full_name() == "altstrategy.v1.TradeSignalProposal");
}

TEST_CASE("the envelope's payload numbers are frozen") {
    const std::string m = "altsignalquality.v1.Envelope";
    require_field(m, "schema_version", 1);
    require_field(m, "message_id", 2);
    require_field(m, "source_service", 9);
    require_field(m, "final_trade_signal", 20);
    require_field(m, "quality_evaluation", 21);
    require_field(m, "model_event", 22);
}

TEST_CASE("decision enums keep their meaning") {
    REQUIRE(altsignalquality::v1::DECISION_ACCEPTED == 1);
    REQUIRE(altsignalquality::v1::DECISION_REJECTED == 2);
    REQUIRE(altsignalquality::v1::BASIS_MODEL == 1);
    REQUIRE(altsignalquality::v1::BASIS_COLD_START == 2);
    REQUIRE(altsignalquality::v1::BASIS_MODEL_UNUSABLE == 3);
    REQUIRE(altsignalquality::v1::BASIS_UPSTREAM_NOT_RECOMMENDED == 4);
    REQUIRE(altsignalquality::v1::BASIS_MALFORMED_SIGNAL == 5);
    REQUIRE(altsignalquality::v1::BASIS_SCORE_BELOW_FLOOR == 6);
    REQUIRE(altsignalquality::v1::BASIS_SIGNAL_STALE == 7);
}

TEST_CASE("the vendored altstrategy.v1 fields this service reads are at their numbers") {
    const std::string m = "altstrategy.v1.TradeSignalProposal";
    require_field(m, "signal_proposal_id", 1);
    require_field(m, "candidate_id", 2);
    require_field(m, "canonical_pair_id", 4);
    require_field(m, "strategy_id", 6);
    require_field(m, "side", 9);
    require_field(m, "proposal_grade", 10);
    require_field(m, "created_at_ms", 11);
    require_field(m, "entry_price", 14);
    require_field(m, "stop_loss_price", 23);
    require_field(m, "take_profit_price", 27);
    require_field(m, "p_target_before_stop", 36);
    require_field(m, "expected_net_return", 41);
    require_field(m, "expected_rr", 43);
    require_field(m, "adaptive_score", 49);
    require_field(m, "recommended_for_execution", 54);
    require_field(m, "context_revision", 56);
    require_field(m, "feature_manifest_hash", 57);
    // The trade proposal deliberately carries no context_id; this service
    // resolves it from the candidate, and that fact is worth pinning.
    REQUIRE(desc(m)->FindFieldByName("context_id") == nullptr);
    require_field("altstrategy.v1.Envelope", "trade_signal_proposal", 24);
}

TEST_CASE("a candidate round-trips through the parse path") {
    altstrategy::v1::Envelope env;
    env.set_schema_version(1);
    env.set_symbol("BTCUSDT");
    env.set_trace_id("trace-1");
    auto* p = env.mutable_trade_signal_proposal();
    p->set_signal_proposal_id("sig-1");
    p->set_side(altstrategy::v1::SIDE_SHORT);
    p->set_recommended_for_execution(true);
    std::string bytes;
    env.SerializeToString(&bytes);

    std::string symbol, trace;
    auto parsed = altsignalquality::messaging::parse_candidate(bytes, &symbol, &trace);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->signal_proposal_id() == "sig-1");
    REQUIRE(parsed->side() == altstrategy::v1::SIDE_SHORT);
    REQUIRE(parsed->recommended_for_execution());
    REQUIRE(symbol == "BTCUSDT");
    REQUIRE(trace == "trace-1");
}

TEST_CASE("a payload that is not a proposal is reported as absent, not as an error") {
    // AltStrategy's stream legitimately carries other payloads; a consumer must
    // skip them without treating the message as corrupt.
    altstrategy::v1::Envelope env;
    env.set_schema_version(1);
    env.mutable_market_outcome()->set_outcome_id("out-1");
    std::string bytes;
    env.SerializeToString(&bytes);
    auto parsed = altsignalquality::messaging::parse_candidate(bytes, nullptr, nullptr);
    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(parsed.error().code == altsignalquality::ErrorCode::not_found);
}

TEST_CASE("a wrong schema version is refused rather than misread") {
    altstrategy::v1::Envelope env;
    env.set_schema_version(99);
    env.mutable_trade_signal_proposal()->set_signal_proposal_id("sig-1");
    std::string bytes;
    env.SerializeToString(&bytes);
    auto parsed = altsignalquality::messaging::parse_candidate(bytes, nullptr, nullptr);
    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(parsed.error().code == altsignalquality::ErrorCode::validation_error);
}

TEST_CASE("this service's own envelope round-trips") {
    altsignalquality::messaging::EnvelopeMeta meta;
    meta.message_id = "msg-1";
    meta.event_type = "final_trade_signal";
    meta.canonical_pair_id = 7;
    meta.symbol = "ETHUSDT";
    auto env = altsignalquality::messaging::make_envelope(meta);
    env.mutable_final_trade_signal()->set_final_signal_id("fin-1");
    const auto bytes = altsignalquality::messaging::serialize(env);
    auto parsed = altsignalquality::messaging::parse_envelope(bytes);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->source_service() == "altsignalquality");
    REQUIRE(parsed->has_final_trade_signal());
    REQUIRE(parsed->final_trade_signal().final_signal_id() == "fin-1");
    REQUIRE(parsed->produced_at_ms() > 0);
}

// -----------------------------------------------------------------------------
// Defect: the highest-priority refusal had no wire value.
//
// `decide()` checks the holding ceiling before everything else, staleness
// included, and rejects the signal whatever a model would have scored it. The
// proto enum had no member for it, so `basis_to_wire`'s switch fell through to
// DECISION_BASIS_UNSPECIFIED and the evaluation stream -- the audit trail --
// recorded the reason as "unspecified". Every basis must have a distinct wire
// value, so the mapping cannot silently lose one again.
// -----------------------------------------------------------------------------
TEST_CASE("every decision basis has a wire value") {
    using altsignalquality::domain::DecisionBasis;
    const std::array<DecisionBasis, 8> all{
        DecisionBasis::model,            DecisionBasis::cold_start,
        DecisionBasis::model_unusable,   DecisionBasis::upstream_not_recommended,
        DecisionBasis::malformed_signal, DecisionBasis::score_below_floor,
        DecisionBasis::signal_stale,     DecisionBasis::holding_exceeds_ceiling};

    const auto* e = google::protobuf::DescriptorPool::generated_pool()->FindEnumTypeByName(
        "altsignalquality.v1.DecisionBasis");
    REQUIRE(e != nullptr);
    // One wire value per basis, plus UNSPECIFIED.
    REQUIRE(static_cast<size_t>(e->value_count()) == all.size() + 1);

    for (const auto b : all) {
        const std::string wire_name =
            "BASIS_" + std::string(altsignalquality::domain::decision_basis_name(b));
        INFO("basis " << wire_name);
        const auto* v = e->FindValueByName(wire_name);
        REQUIRE(v != nullptr);
        REQUIRE(v->number() != 0);  // never UNSPECIFIED
    }
}
