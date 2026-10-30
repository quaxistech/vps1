#include "altsignalquality/components/publication_evidence.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <fstream>
#include <sstream>
using namespace altsignalquality;
namespace {
std::string text(const std::string& path) {
    std::ifstream input(path); std::ostringstream out; out << input.rdbuf();
    if (!input) throw std::runtime_error("fixture source unavailable");
    return out.str();
}
}

TEST_CASE("first publication claim survives retry, context change and restart") {
    const char* dsn = std::getenv("ALTSQ_TEST_PG_DSN");
    if (!dsn) SKIP("dedicated local test database required");
    pqxx::connection observer(dsn);
    REQUIRE(std::string(observer.dbname()).starts_with("altsq_audit_test_"));
    REQUIRE(std::string(observer.hostname()).starts_with("/"));
    {
        pqxx::work tx(observer);
        tx.exec("DROP SCHEMA IF EXISTS altsignalquality CASCADE");
        tx.exec(text(std::string(ALTSQ_SOURCE_DIR) + "/db/migrations/0001_altsignalquality_schema.sql"));
        tx.exec(text(std::string(ALTSQ_SOURCE_DIR) + "/db/migrations/0002_publication_evidence.sql"));
        tx.exec(text(std::string(ALTSQ_SOURCE_DIR) + "/db/migrations/0002_publication_evidence.sql"));
        tx.commit();
    }
    persistence::PgPool pool(dsn,2);
    data::Store store(pool,"altstrategy");
    for (const bool scored : {false,true}) {
        data::EvaluationRow row;
        row.signal_proposal_id = scored ? "fixture-scored" : "fixture-cold";
        row.evaluation_id = row.signal_proposal_id + "-evaluation";
        row.final_signal_id = row.signal_proposal_id + "-final";
        row.candidate_id = "fixture-candidate"; row.canonical_pair_id = 42;
        row.symbol = "FIXTURE"; row.strategy_id = "fixture"; row.strategy_version = "1";
        row.side = "long"; row.proposal_grade = "NORMAL"; row.context_revision = 5;
        row.signal_created_at_ms = 123000; row.evaluated_at_ms = 123456;
        row.feature_schema_version = "fixture"; row.feature_schema_hash = "fixture-hash";
        row.features.values.assign(domain::kFeatureCount,0.25f);
        row.features.validity.assign((domain::kFeatureCount+7)/8,0xff);
        row.has_quality_score = scored; row.raw_score = row.quality_score = .77;
        row.decision_threshold = .5; row.decision = "ACCEPTED";
        row.decision_basis = scored ? "MODEL" : "COLD_START";
        row.model_id = scored ? "original-model" : "";
        altstrategy::v1::Envelope input; input.set_schema_version(1); input.set_symbol(row.symbol);
        auto* proposal = input.mutable_trade_signal_proposal();
        proposal->set_signal_proposal_id(row.signal_proposal_id); proposal->set_entry_price(100);
        proposal->set_stop_loss_price(99); proposal->set_take_profit_price(102);
        row.candidate_payload = input.SerializeAsString(); row.candidate_sha256 = sha256_hex(row.candidate_payload);
        altsignalquality::v1::Envelope publication; publication.set_schema_version(1);
        auto* final = publication.mutable_final_trade_signal();
        final->set_final_signal_id(row.final_signal_id); *final->mutable_proposal() = *proposal;
        auto* eval = final->mutable_quality();
        eval->set_evaluation_id(row.evaluation_id); eval->set_signal_proposal_id(row.signal_proposal_id);
        eval->set_evaluated_at_ms(row.evaluated_at_ms); eval->set_decision(altsignalquality::v1::DECISION_ACCEPTED);
        eval->set_basis(scored ? altsignalquality::v1::BASIS_MODEL : altsignalquality::v1::BASIS_COLD_START);
        eval->set_model_id(row.model_id); eval->set_model_version(scored ? 17 : 0);
        eval->set_has_quality_score(scored); eval->set_quality_score(row.quality_score); eval->add_reason_codes("original-reason");
        final->set_published_at_ms(123900); final->set_producer_version("original-producer");
        row.publication_payload = publication.SerializeAsString();
        const auto claimed = store.claim_evaluation(row);
        REQUIRE(claimed); CHECK_FALSE(claimed->has_value());
        // Simulate the committed claim / failed publish window. A fresh Store
        // reads database evidence, with no dependency on the previous object.
        data::Store restored(pool,"altstrategy");
        const auto pending = restored.load_unpublished(500);
        REQUIRE(pending);
        auto next = row; next.model_id = "changed-model"; next.quality_score = .01;
        next.context_revision = 999; next.features.values[0] = 99;
        next.publication_payload = "must never replace the first publication";
        const auto duplicate = restored.claim_evaluation(next);
        REQUIRE(duplicate); REQUIRE(duplicate->has_value());
        CHECK((**duplicate).publication_payload == row.publication_payload);
        CHECK((**duplicate).candidate_payload == row.candidate_payload);
        REQUIRE(restore_publication(**duplicate));
        {
            pqxx::read_transaction tx(observer);
            const auto result = tx.exec_params("SELECT context_revision,features[1] FROM "
                "altsignalquality.signal_evaluations WHERE signal_proposal_id=$1",row.signal_proposal_id);
            CHECK(result[0][0].as<int>() == 5); CHECK(result[0][1].as<float>() == .25f);
        }
        REQUIRE(store.mark_published(row.signal_proposal_id,124000));
        const auto published = store.find_evaluation(row.signal_proposal_id);
        REQUIRE(published); REQUIRE(*published); CHECK((**published).published);
        CHECK((**published).candidate_payload.empty());
        CHECK((**published).publication_payload == row.publication_payload);
        CHECK(redelivery_matches(**published,row.candidate_payload));
        input.mutable_trade_signal_proposal()->set_entry_price(101);
        CHECK_FALSE(redelivery_matches(**published,input.SerializeAsString()));
    }
}
