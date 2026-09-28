// The wire contract.
//
// Two things are frozen here, and both are frozen for the same reason: a
// consumer has shipped against them.
//
//   * FIELD NUMBERS on the owned contract. Renumbering a field silently
//     reinterprets every message a running consumer decodes.
//   * THE ENUM IDENTITIES this contract reuses. The canonical signal does not
//     restate Side, EntryType, QualityDecision and the rest — it imports the
//     producers' own types. This file asserts that the reuse is real, so a
//     future "let's have our own enum" refactor fails here rather than in
//     production on the one value that moved.

#include "gateway.pb.h"

#include <catch2/catch_test_macros.hpp>

#include <google/protobuf/descriptor.h>

#include <set>
#include <string>

namespace pb = altsignalgateway::v1;
namespace as = altstrategy::v1;
namespace sq = altsignalquality::v1;

namespace {

int field_number(const google::protobuf::Descriptor* d, const std::string& name) {
    const auto* f = d->FindFieldByName(name);
    REQUIRE(f != nullptr);
    return f->number();
}

std::string field_type_name(const google::protobuf::Descriptor* d, const std::string& name) {
    const auto* f = d->FindFieldByName(name);
    REQUIRE(f != nullptr);
    if (f->type() == google::protobuf::FieldDescriptor::TYPE_ENUM) {
        return f->enum_type()->full_name();
    }
    if (f->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
        return f->message_type()->full_name();
    }
    return f->type_name();
}

} // namespace

TEST_CASE("the transport envelope's field numbers are frozen") {
    const auto* d = pb::SignalEnvelope::descriptor();
    CHECK(field_number(d, "schema_version") == 1);
    CHECK(field_number(d, "protocol_version") == 2);
    CHECK(field_number(d, "message_id") == 3);
    CHECK(field_number(d, "signal_id") == 4);
    CHECK(field_number(d, "sequence") == 5);
    CHECK(field_number(d, "signal_type") == 6);
    CHECK(field_number(d, "created_at_ms") == 7);
    CHECK(field_number(d, "expires_at_ms") == 8);
    CHECK(field_number(d, "journaled_at_ms") == 9);
    CHECK(field_number(d, "published_at_ms") == 10);
    CHECK(field_number(d, "payload_sha256") == 11);
    CHECK(field_number(d, "producer_service") == 12);
    CHECK(field_number(d, "producer_version") == 13);
    CHECK(field_number(d, "replay") == 14);
    CHECK(field_number(d, "delivery_attempt") == 15);
}

TEST_CASE("the payload's block numbering is frozen") {
    const auto* d = pb::SignalPayload::descriptor();
    CHECK(field_number(d, "trade_plan") == 1);
    CHECK(field_number(d, "strategy") == 2);
    CHECK(field_number(d, "altml") == 3);
    CHECK(field_number(d, "signal_quality") == 4);
    CHECK(field_number(d, "market_snapshot") == 5);
    CHECK(field_number(d, "microstructure") == 6);
    CHECK(field_number(d, "regime") == 7);
    CHECK(field_number(d, "execution_context") == 8);
    CHECK(field_number(d, "provenance") == 9);
    CHECK(field_number(d, "source_envelope") == 20);
}

TEST_CASE("the trade plan's executable fields are frozen") {
    const auto* d = pb::TradePlan::descriptor();
    CHECK(field_number(d, "exchange") == 1);
    CHECK(field_number(d, "symbol") == 2);
    CHECK(field_number(d, "side") == 10);
    CHECK(field_number(d, "entry_type") == 11);
    CHECK(field_number(d, "entry_price") == 12);
    CHECK(field_number(d, "stop_loss_price") == 30);
    CHECK(field_number(d, "take_profit_price") == 34);
    CHECK(field_number(d, "valid_until_ms") == 55);
}

TEST_CASE("enums are the producers' own types, not local copies") {
    // If any of these ever names an altsignalgateway.v1 type, someone has
    // restated a producer's enum and the two can now drift.
    CHECK(field_type_name(pb::TradePlan::descriptor(), "side") == "altstrategy.v1.Side");
    CHECK(field_type_name(pb::TradePlan::descriptor(), "entry_type") ==
          "altstrategy.v1.EntryType");
    CHECK(field_type_name(pb::TradePlan::descriptor(), "holding_class") ==
          "altstrategy.v1.HoldingClass");
    CHECK(field_type_name(pb::StrategyBlock::descriptor(), "proposal_grade") ==
          "altstrategy.v1.ProposalGrade");
    CHECK(field_type_name(pb::StrategyBlock::descriptor(), "state") ==
          "altstrategy.v1.ProposalState");
    CHECK(field_type_name(pb::StrategyBlock::descriptor(), "context_quality") ==
          "altstrategy.v1.ContextQuality");
    CHECK(field_type_name(pb::StrategyBlock::descriptor(), "warnings") ==
          "altstrategy.v1.ProposalWarning");
    CHECK(field_type_name(pb::SignalQualityBlock::descriptor(), "decision") ==
          "altsignalquality.v1.QualityDecision");
    CHECK(field_type_name(pb::SignalQualityBlock::descriptor(), "basis") ==
          "altsignalquality.v1.DecisionBasis");
    CHECK(field_type_name(pb::MarketSnapshot::descriptor(), "data_quality") ==
          "altstrategy.v1.ContextQuality");
}

TEST_CASE("the vendored enum values are the ones this build was written against") {
    // Numeric values, not names: a reordering that keeps the names would
    // silently remap every message already on the wire.
    CHECK(static_cast<int>(as::SIDE_LONG) == 1);
    CHECK(static_cast<int>(as::SIDE_SHORT) == 2);
    CHECK(static_cast<int>(as::ENTRY_MARKET) == 1);
    CHECK(static_cast<int>(as::ENTRY_LIMIT) == 2);
    CHECK(static_cast<int>(as::ENTRY_STOP_MARKET) == 3);
    CHECK(static_cast<int>(as::ENTRY_STOP_LIMIT) == 4);
    CHECK(static_cast<int>(as::GRADE_INVALID) == 6);
    CHECK(static_cast<int>(as::HOLDING_INTRADAY) == 2);
    CHECK(static_cast<int>(as::CONTEXT_STRICT_4_OF_4) == 1);
    CHECK(static_cast<int>(sq::DECISION_ACCEPTED) == 1);
    CHECK(static_cast<int>(sq::DECISION_REJECTED) == 2);
    CHECK(static_cast<int>(sq::BASIS_MODEL) == 1);
    CHECK(static_cast<int>(sq::BASIS_COLD_START) == 2);
}

TEST_CASE("the upstream contract still carries what this service reads") {
    // A field this service depends on disappearing upstream would otherwise
    // show up as a silently empty block in a delivered trade.
    const auto* p = as::TradeSignalProposal::descriptor();
    for (const char* name :
         {"signal_proposal_id", "candidate_id", "canonical_pair_id", "strategy_id",
          "strategy_version", "side", "entry_type", "entry_price", "stop_loss_price",
          "take_profit_price", "created_at_ms", "valid_until_ms", "max_holding_time_ms",
          "recommended_for_execution", "context_revision", "model_versions", "ml_conviction",
          "ml_abstain", "edge_estimated", "p_target_before_stop", "expected_net_return",
          "proposal_grade", "holding_class"}) {
        INFO("altstrategy.v1.TradeSignalProposal." << name);
        CHECK(p->FindFieldByName(name) != nullptr);
    }
    // The mirror of the check above: a name the producer RESERVED must not
    // reappear here. It did once -- the vendored copy was ten days stale and
    // still declared ml_uncertainty/ml_ood, so this service read protobuf zeros
    // and shipped them as measurements. The byte-identity test catches the
    // drift; this states why it matters.
    for (const char* gone : {"ml_uncertainty", "ml_ood", "p_entry_reached",
                             "p_stop_before_target", "p_timeout", "p_ambiguous",
                             "expected_gross_return", "expected_utility"}) {
        INFO("reserved upstream: altstrategy.v1.TradeSignalProposal." << gone);
        CHECK(p->FindFieldByName(gone) == nullptr);
    }

    const auto* q = sq::SignalQualityEvaluation::descriptor();
    for (const char* name :
         {"evaluation_id", "decision", "basis", "has_quality_score", "quality_score",
          "decision_threshold", "model_id", "model_version", "feature_schema_hash", "context_id",
          "signal_created_at_ms", "evaluated_at_ms"}) {
        INFO("altsignalquality.v1.SignalQualityEvaluation." << name);
        CHECK(q->FindFieldByName(name) != nullptr);
    }
    const auto* f = sq::FinalTradeSignal::descriptor();
    CHECK(f->FindFieldByName("final_signal_id") != nullptr);
    CHECK(f->FindFieldByName("proposal") != nullptr);
    CHECK(f->FindFieldByName("quality") != nullptr);
    CHECK(f->FindFieldByName("producer_version") != nullptr);
}

TEST_CASE("the protocol frames are frozen") {
    const auto* c = pb::ClientFrame::descriptor();
    CHECK(field_number(c, "hello") == 1);
    CHECK(field_number(c, "auth") == 2);
    CHECK(field_number(c, "resume") == 3);
    CHECK(field_number(c, "ack") == 4);
    CHECK(field_number(c, "heartbeat_ack") == 5);
    CHECK(field_number(c, "goodbye") == 6);

    const auto* s = pb::ServerFrame::descriptor();
    CHECK(field_number(s, "hello_ok") == 1);
    CHECK(field_number(s, "auth_ok") == 2);
    CHECK(field_number(s, "resume_ok") == 3);
    CHECK(field_number(s, "signal") == 4);
    CHECK(field_number(s, "resume_complete") == 5);
    CHECK(field_number(s, "ready") == 6);
    CHECK(field_number(s, "heartbeat") == 7);
    CHECK(field_number(s, "error") == 8);
    CHECK(field_number(s, "goodbye") == 9);
}

TEST_CASE("the canonical payload contains no map fields") {
    // Map ordering is unspecified on the wire, and the integrity hash is taken
    // over the serialized payload. One map anywhere in this tree would make the
    // hash unreproducible by an external consumer.
    std::set<const google::protobuf::Descriptor*> seen;
    const auto walk = [&seen](const google::protobuf::Descriptor* d, auto&& self) -> void {
        if (d == nullptr || !seen.insert(d).second) return;
        for (int i = 0; i < d->field_count(); ++i) {
            const auto* f = d->field(i);
            INFO(d->full_name() << "." << f->name());
            CHECK_FALSE(f->is_map());
            if (f->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
                self(f->message_type(), self);
            }
        }
    };
    walk(pb::SignalPayload::descriptor(), walk);
}

TEST_CASE("the resume modes cannot be defaulted into") {
    // RESUME_MODE_UNSPECIFIED must stay 0 and must stay meaningless: "start
    // where I left off" and "send me everything" differ by an entire journal.
    CHECK(static_cast<int>(pb::RESUME_MODE_UNSPECIFIED) == 0);
    CHECK(static_cast<int>(pb::RESUME_FROM_SEQUENCE) == 1);
    CHECK(static_cast<int>(pb::RESUME_LIVE_ONLY) == 2);
    CHECK(static_cast<int>(pb::RESUME_FROM_START) == 3);
}
