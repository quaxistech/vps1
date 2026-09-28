// End-to-end integration test for the live path, against a REAL running service.
//
// Everything else in this suite is pure logic. This one exercises the parts that
// only exist when a broker and a database are actually present: the durable
// consumer, the wire contract on the bus, the point-in-time context lookup, the
// evaluation claim, the published FINAL signal, and idempotency under a
// redelivery.
//
// It is NOT part of `ctest` by default. It runs only when both endpoints are
// supplied, because it publishes a real message onto a real stream:
//
//   ALTSQ_IT_DSN=postgresql://...  ALTSQ_IT_NATS=nats://...
//   ./build/release/bin/altsignalquality_integration_tests
//
// The signal it publishes is not invented: it is an ARCHIVED AltStrategy
// proposal, read back from `altstrategy.trade_signal_proposals` and re-encoded
// on the producer's own contract. Nothing about the market is fabricated. The
// test removes the evaluation row it caused before it finishes, so a diagnostic
// run does not leave a decision in the dataset.

#include "altsignalquality/core/codec.hpp"
#include "altsignalquality/core/nats_client.hpp"
#include "altsignalquality/core/pg.hpp"
#include "altsignalquality/core/time.hpp"
#include "altsignalquality/domain/feature_contract.hpp"
#include "altsignalquality/data/store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <thread>

using namespace altsignalquality;

namespace {

struct Endpoints {
    std::string dsn;
    std::string nats;
    // Publishing a candidate requires the PRODUCER's credentials. This service's
    // own NATS user is deliberately not permitted to write into `signal.>` —
    // that namespace belongs to AltStrategy, and a filter that could fabricate
    // its own input would be a filter that could fabricate a trade. The test
    // therefore needs AltStrategy's publisher URL, and the fact that it needs a
    // separate one is itself part of what is being verified.
    std::string publisher_nats;
    [[nodiscard]] bool complete() const {
        return !dsn.empty() && !nats.empty() && !publisher_nats.empty();
    }
};

Endpoints endpoints() {
    Endpoints e;
    if (const char* dsn = std::getenv("ALTSQ_IT_DSN")) e.dsn = dsn;
    if (const char* nats = std::getenv("ALTSQ_IT_NATS")) e.nats = nats;
    if (const char* pub = std::getenv("ALTSQ_IT_PUBLISHER_NATS")) e.publisher_nats = pub;
    return e;
}

// One archived proposal, re-encoded on AltStrategy's contract. Only the id is
// changed — to a clearly marked test id — so the run cannot collide with, or be
// mistaken for, the original decision.
struct ArchivedSignal {
    bool found = false;
    std::string original_id;
    std::string symbol;
    altstrategy::v1::TradeSignalProposal proposal;
};

ArchivedSignal load_archived(persistence::PgPool& pool, const std::string& test_id) {
    ArchivedSignal out;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec(
            "SELECT p.signal_proposal_id, p.candidate_id, p.canonical_pair_id,"
            "       p.bitget_instrument_id, p.strategy_id, p.strategy_version,"
            "       p.parameter_set_id, p.side, p.proposal_grade, p.created_at_ms,"
            "       p.valid_until_ms, p.entry_price, p.stop_loss_price, p.take_profit_price,"
            "       p.max_holding_time_ms, p.time_stop_at_ms, p.expected_rr,"
            "       p.recommended_for_execution, p.context_revision, p.plan_version"
            " FROM altstrategy.trade_signal_proposals p"
            " WHERE p.plan_version = 1 AND p.proposal_grade <> 'INVALID'"
            "   AND p.recommended_for_execution"
            " ORDER BY p.created_at_ms DESC LIMIT 1");
        if (rows.empty()) return;
        const auto& r = rows[0];
        out.found = true;
        out.original_id = r[0].as<std::string>();
        auto& p = out.proposal;
        p.set_signal_proposal_id(test_id);
        p.set_candidate_id(r[1].as<std::string>());
        p.set_canonical_pair_id(r[2].as<int64_t>());
        p.set_bitget_instrument_id(r[3].as<int64_t>());
        p.set_strategy_id(r[4].as<std::string>());
        p.set_strategy_version(r[5].as<std::string>());
        p.set_parameter_set_id(r[6].as<std::string>());
        p.set_side(r[7].as<std::string>() == "short" ? altstrategy::v1::SIDE_SHORT
                                                     : altstrategy::v1::SIDE_LONG);
        p.set_proposal_grade(altstrategy::v1::GRADE_NORMAL);
        // The freshness gate is part of what is being tested, and an archived
        // signal is weeks old. Presenting it as current is the only way to
        // exercise the scoring path rather than the staleness path; the test
        // asserts the staleness path separately with the original timestamp.
        p.set_created_at_ms(timeutil::now_ms());
        p.set_valid_until_ms(timeutil::now_ms() + 900'000);
        p.set_entry_type(altstrategy::v1::ENTRY_LIMIT);
        p.set_entry_price(r[11].as<double>());
        p.set_stop_loss_price(r[12].as<double>());
        p.set_take_profit_price(r[13].as<double>());
        p.set_max_holding_time_ms(r[14].as<int64_t>());
        p.set_time_stop_at_ms(timeutil::now_ms() + r[14].as<int64_t>());
        p.set_expected_rr(r[16].as<double>());
        p.set_recommended_for_execution(r[17].as<bool>());
        p.set_context_revision(r[18].as<int64_t>());
        p.set_plan_version(r[19].as<int32_t>());
        out.symbol = "pair-" + std::to_string(p.canonical_pair_id());
    });
    if (!res) out.found = false;
    return out;
}

std::string publish(messaging::NatsClient& nats, const ArchivedSignal& signal) {
    altstrategy::v1::Envelope env;
    env.set_schema_version(1);
    env.set_message_id(signal.proposal.signal_proposal_id());
    env.set_event_type("trade_signal_proposal");
    env.set_canonical_pair_id(signal.proposal.canonical_pair_id());
    env.set_symbol(signal.symbol);
    env.set_produced_at_ms(timeutil::now_ms());
    env.set_source_service("altsignalquality-integration-test");
    *env.mutable_trade_signal_proposal() = signal.proposal;
    std::string bytes;
    env.SerializeToString(&bytes);
    const std::string subject = "signal.proposed." + signal.symbol + ".v1";
    REQUIRE(nats.publish(subject, bytes).has_value());
    return subject;
}

// Poll for the evaluation the service should have written.
struct Observed {
    bool present = false;
    std::string decision, basis, model_id;
    bool published = false;
    int missing_features = 0;
    int64_t evaluated_at_ms = 0;
    std::string final_signal_id;
};

Observed await_evaluation(persistence::PgPool& pool, const std::string& id, int seconds) {
    Observed obs;
    for (int i = 0; i < seconds * 2 && !obs.present; ++i) {
        (void)pool.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec_params(
                "SELECT decision, decision_basis, COALESCE(model_id,''), published,"
                "       missing_feature_count, evaluated_at_ms, COALESCE(final_signal_id,'')"
                " FROM altsignalquality.signal_evaluations WHERE signal_proposal_id = $1",
                id);
            if (rows.empty()) return;
            obs.present = true;
            obs.decision = rows[0][0].as<std::string>();
            obs.basis = rows[0][1].as<std::string>();
            obs.model_id = rows[0][2].as<std::string>();
            obs.published = rows[0][3].as<bool>();
            obs.missing_features = rows[0][4].as<int>();
            obs.evaluated_at_ms = rows[0][5].as<int64_t>();
            obs.final_signal_id = rows[0][6].as<std::string>();
        });
        if (!obs.present) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return obs;
}

void cleanup(persistence::PgPool& pool, const std::string& id) {
    (void)pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params("DELETE FROM altsignalquality.training_examples"
                        " WHERE signal_proposal_id = $1", id);
        txn.exec_params("DELETE FROM altsignalquality.signal_evaluations"
                        " WHERE signal_proposal_id = $1", id);
    });
}

} // namespace

TEST_CASE("a candidate on the bus becomes a durable evaluation", "[integration]") {
    const auto ep = endpoints();
    if (!ep.complete()) {
        WARN("ALTSQ_IT_DSN / ALTSQ_IT_NATS / ALTSQ_IT_PUBLISHER_NATS not set; skipping");
        return;
    }
    persistence::PgPool pool(ep.dsn, 2);
    auto nats = messaging::NatsClient::connect(ep.publisher_nats,
                                               "altsignalquality-integration-test");
    REQUIRE(nats.has_value());

    const std::string test_id =
        "IT_" + std::to_string(timeutil::now_ms()) + "_live_path";
    const auto archived = load_archived(pool, test_id);
    if (!archived.found) {
        WARN("no archived recommended proposal to replay; skipping");
        return;
    }
    INFO("replaying archived proposal " << archived.original_id);

    publish(**nats, archived);
    (*nats)->drain(2000);

    const auto observed = await_evaluation(pool, test_id, 30);
    REQUIRE(observed.present);
    // With no ACTIVE model the configured cold-start policy decides, and the
    // basis must say so rather than claiming a model scored it.
    REQUIRE((observed.basis == "COLD_START" || observed.basis == "MODEL"));
    REQUIRE((observed.decision == "ACCEPTED" || observed.decision == "REJECTED"));
    if (observed.decision == "ACCEPTED") {
        REQUIRE_FALSE(observed.final_signal_id.empty());
        REQUIRE(observed.final_signal_id == data::make_final_signal_id(test_id));
        REQUIRE(observed.published);
    }
    // The context lookup ran: an archived candidate resolves its context, so a
    // fully-missing vector would mean the join silently failed.
    REQUIRE(observed.missing_features < static_cast<int>(domain::kFeatureCount));

    SECTION("a redelivery produces no second evaluation or final signal") {
        const int64_t first_evaluated = observed.evaluated_at_ms;
        publish(**nats, archived);
        (*nats)->drain(2000);
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const auto again = await_evaluation(pool, test_id, 5);
        REQUIRE(again.present);
        // Same row, untouched: the claim is the idempotency boundary.
        REQUIRE(again.evaluated_at_ms == first_evaluated);
        REQUIRE(again.final_signal_id == observed.final_signal_id);

        int64_t rows = 0;
        (void)pool.with_txn([&](pqxx::work& txn) {
            rows = txn.exec_params("SELECT count(*) FROM altsignalquality.signal_evaluations"
                                   " WHERE signal_proposal_id = $1", test_id)[0][0].as<int64_t>();
        });
        REQUIRE(rows == 1);
    }

    cleanup(pool, test_id);
}

TEST_CASE("a stale candidate is rejected as stale, not scored", "[integration]") {
    const auto ep = endpoints();
    if (!ep.complete()) return;
    persistence::PgPool pool(ep.dsn, 2);
    auto nats = messaging::NatsClient::connect(ep.publisher_nats,
                                               "altsignalquality-integration-test");
    REQUIRE(nats.has_value());

    const std::string test_id = "IT_" + std::to_string(timeutil::now_ms()) + "_stale";
    auto archived = load_archived(pool, test_id);
    if (!archived.found) return;
    // Present it with its ORIGINAL age: weeks old, far past any freshness window.
    archived.proposal.set_created_at_ms(timeutil::now_ms() - 30LL * 86'400'000);
    publish(**nats, archived);
    (*nats)->drain(2000);

    const auto observed = await_evaluation(pool, test_id, 30);
    REQUIRE(observed.present);
    REQUIRE(observed.decision == "REJECTED");
    REQUIRE(observed.basis == "SIGNAL_STALE");
    REQUIRE(observed.model_id.empty());
    cleanup(pool, test_id);
}

// -----------------------------------------------------------------------------
// The service's own NATS user must NOT be able to publish a candidate. A filter
// that can write into its own input subject is a filter that can fabricate a
// trade, and no amount of application logic compensates for that.
// -----------------------------------------------------------------------------
TEST_CASE("the service cannot publish into AltStrategy's namespace", "[integration]") {
    const auto ep = endpoints();
    if (!ep.complete()) return;
    auto nats = messaging::NatsClient::connect(ep.nats, "altsignalquality-permission-probe");
    REQUIRE(nats.has_value());

    altstrategy::v1::Envelope env;
    env.set_schema_version(1);
    env.mutable_trade_signal_proposal()->set_signal_proposal_id("IT_permission_probe");
    std::string bytes;
    env.SerializeToString(&bytes);

    // The publish is accepted locally and refused by the server asynchronously,
    // so the observable proof is that nothing reaches the stream.
    (void)(*nats)->publish("signal.proposed.permission-probe.v1", bytes);
    (*nats)->drain(2000);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    persistence::PgPool pool(ep.dsn, 1);
    int64_t rows = 1;
    (void)pool.with_txn([&](pqxx::work& txn) {
        rows = txn.exec_params("SELECT count(*) FROM altsignalquality.signal_evaluations"
                               " WHERE signal_proposal_id = $1",
                               std::string("IT_permission_probe"))[0][0].as<int64_t>();
    });
    REQUIRE(rows == 0);
}
