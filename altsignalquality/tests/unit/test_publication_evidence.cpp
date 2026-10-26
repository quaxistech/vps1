#include "altsignalquality/components/publication_evidence.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace altsignalquality;

TEST_CASE("pending publication restores original evidence without current model or policy") {
    altstrategy::v1::Envelope input;
    input.set_schema_version(1);
    input.set_symbol("BTCUSDT");
    input.set_trace_id("original-trace");
    auto* p = input.mutable_trade_signal_proposal();
    p->set_signal_proposal_id("original-proposal");
    p->set_candidate_id("original-candidate");
    p->set_entry_price(100);
    p->set_stop_loss_price(99);
    p->set_take_profit_price(102);

    data::ExistingEvaluation row;
    row.evaluation_id = "original-evaluation";
    row.final_signal_id = "original-final";
    row.decision = "ACCEPTED";
    row.evaluated_at_ms = 123456;
    row.candidate_payload = input.SerializeAsString();
    row.candidate_sha256 = sha256_hex(row.candidate_payload);
    for (const bool have_model : {false, true}) {
        altsignalquality::v1::SignalQualityEvaluation eval;
        eval.set_evaluation_id(row.evaluation_id);
        eval.set_signal_proposal_id(p->signal_proposal_id());
        eval.set_evaluated_at_ms(row.evaluated_at_ms);
        eval.set_decision(altsignalquality::v1::DECISION_ACCEPTED);
        eval.set_basis(have_model ? altsignalquality::v1::BASIS_MODEL :
                                  altsignalquality::v1::BASIS_COLD_START);
        eval.set_model_id(have_model ? "original-model" : "");
        eval.set_model_version(have_model ? 17 : 0);
        eval.set_has_quality_score(have_model);
        eval.set_quality_score(0.77);
        eval.add_reason_codes("original-reason");
        altsignalquality::v1::Envelope final;
        final.set_schema_version(1);
        final.set_message_id(row.final_signal_id);
        auto* signal = final.mutable_final_trade_signal();
        signal->set_final_signal_id(row.final_signal_id);
        *signal->mutable_proposal() = *p;
        *signal->mutable_quality() = eval;
        signal->set_published_at_ms(123999);
        signal->set_producer_version("original-version");
        row.publication_payload = final.SerializeAsString();
        // Both process restart and redelivery use this same restore contract.
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto restored = restore_publication(row);
            REQUIRE(restored);
            CHECK(restored->evaluation.SerializeAsString() == eval.SerializeAsString());
            CHECK(restored->proposal.SerializeAsString() == p->SerializeAsString());
            CHECK(restored->trace_id == "original-trace");
        }
        CHECK(redelivery_matches(row, row.candidate_payload));
        auto conflict = input;
        conflict.mutable_trade_signal_proposal()->set_entry_price(101);
        CHECK_FALSE(redelivery_matches(row, conflict.SerializeAsString()));
        // The digest survives clearing the pending input after publication.
        auto published = row; published.candidate_payload.clear(); published.published = true;
        CHECK_FALSE(redelivery_matches(published, conflict.SerializeAsString()));
        CHECK(redelivery_matches(published, row.candidate_payload));
        published.candidate_sha256.clear();
        CHECK_FALSE(redelivery_matches(published, row.candidate_payload));
    }
    row.publication_payload.clear();
    CHECK_FALSE(restore_publication(row)); // legacy evidence is never guessed
}
