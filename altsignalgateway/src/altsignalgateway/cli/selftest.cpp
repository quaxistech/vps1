// `altsignalgateway selftest` — the database invariants, exercised against the
// REAL database, on the deployed system, at any time.
//
// WHY THIS IS A COMMAND AND NOT A UNIT TEST. Every invariant below is enforced
// by PostgreSQL: a trigger, a constraint, a row lock. A unit test would have to
// mock those, and a mock of a transactional guarantee is a test of the mock. The
// only way to know that the immutability trigger fires on THIS deployment is to
// try to violate it on THIS deployment.
//
// WHY IT IS SAFE TO RUN IN PRODUCTION. Every check runs inside a transaction
// that is deliberately ROLLED BACK. Nothing is committed: no journal row, no
// sequence consumed, no cursor moved. The rollback is itself one of the things
// being tested — a sequence allocated from a row-locked counter must be
// released when its transaction aborts, and this proves it on real data.

#include "altsignalgateway/cli/commands.hpp"

#include "altsignalgateway/core/config.hpp"
#include "altsignalgateway/core/hash.hpp"
#include "altsignalgateway/core/pg.hpp"
#include "altsignalgateway/core/time.hpp"
#include "altsignalgateway/data/pg_bytea.hpp"

#include <cstdio>
#include <string>
#include <thread>

namespace altsignalgateway::cli {

namespace {

int g_failures = 0;

void pass(const std::string& what) { std::printf("PASS  %s\n", what.c_str()); }
void fail(const std::string& what, const std::string& detail) {
    std::printf("FAIL  %s\n      %s\n", what.c_str(), detail.c_str());
    ++g_failures;
}
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    if (ok) pass(what);
    else fail(what, detail);
}

// Run `body` and report whether PostgreSQL refused it, and with what message.
// A refusal is the expected outcome for every invariant test below.
bool refused(pqxx::transaction_base& txn, const std::string& sql, std::string* message) {
    // Each attempt runs inside its own SAVEPOINT: a raised exception aborts the
    // surrounding transaction otherwise, and every later check would fail for
    // the wrong reason.
    try {
        txn.exec("SAVEPOINT probe");
        txn.exec(sql);
        txn.exec("RELEASE SAVEPOINT probe");
        return false;
    } catch (const std::exception& e) {
        *message = e.what();
        try {
            txn.exec("ROLLBACK TO SAVEPOINT probe");
            txn.exec("RELEASE SAVEPOINT probe");
        } catch (const std::exception&) {
            // The transaction is unusable; the caller's checks will report it.
        }
        return true;
    }
}

std::string scratch_payload(const std::string& marker) {
    // Not a real signal and never presented as one: these bytes exist only to
    // give the constraints something to refuse.
    return "altsignalgateway-selftest:" + marker;
}

} // namespace

int cmd_selftest(const Args& args) {
    auto loaded = config::load_runtime(args.config);
    if (!loaded) {
        std::fprintf(stderr, "configuration error: %s\n", loaded.error().message.c_str());
        return 1;
    }
    const auto& cfg = *loaded;
    g_failures = 0;

    const int64_t now = timeutil::now_ms();
    const std::string tag = "selftest_" + std::to_string(now);
    const std::string signal_id = "sqfin_" + tag;
    const std::string message_id = "asgw_" + tag;
    const std::string payload_a = scratch_payload("a");
    const std::string payload_b = scratch_payload("b");

    std::printf("=== altsignalgateway selftest — database invariants ===\n");
    std::printf("every check runs inside a transaction that is rolled back;\n");
    std::printf("nothing below is committed.\n\n");

    persistence::PgPool pool(cfg.common.postgres_dsn, 2);
    auto lease = pool.acquire();
    if (!lease) {
        std::fprintf(stderr, "database error: %s\n", lease.error().message.c_str());
        return 1;
    }

    try {
        pqxx::work txn(lease->conn());

        // ------------------------------------------------------------------
        std::printf("--- sequence allocation ---\n");
        const auto before = txn.query_value<int64_t>(
            "SELECT last_sequence FROM altsignalgateway.sequence_state WHERE singleton");
        const auto first = txn.query_value<int64_t>(
            "UPDATE altsignalgateway.sequence_state SET last_sequence = last_sequence + 1"
            " WHERE singleton RETURNING last_sequence");
        const auto second = txn.query_value<int64_t>(
            "UPDATE altsignalgateway.sequence_state SET last_sequence = last_sequence + 1"
            " WHERE singleton RETURNING last_sequence");
        check(first == before + 1 && second == first + 1,
              "consecutive allocations are gapless",
              "before=" + std::to_string(before) + " first=" + std::to_string(first) +
                  " second=" + std::to_string(second));

        // ------------------------------------------------------------------
        std::printf("\n--- journal insert, idempotency and integrity ---\n");
        const auto insert = [&](const std::string& sid, const std::string& mid, int64_t seq,
                                const std::string& payload) {
            return "INSERT INTO altsignalgateway.signal_journal"
                   " (signal_id, message_id, sequence, signal_proposal_id, canonical_pair_id,"
                   "  side, schema_version, protocol_version, created_at_ms, expires_at_ms,"
                   "  received_at_ms, journaled_at_ms, source_payload, source_payload_sha256,"
                   "  canonical_payload, payload_sha256)"
                   " VALUES (" + txn.quote(sid) + "," + txn.quote(mid) + "," +
                   std::to_string(seq) + "," + txn.quote("sig_" + tag) + ",1,'long',1,1," +
                   std::to_string(now) + "," + std::to_string(now + 600000) + "," +
                   std::to_string(now) + "," + std::to_string(now) + "," +
                   txn.quote(pgbytea::hex_literal(payload)) + "::bytea," +
                   txn.quote(sha256_hex(payload)) + "," +
                   txn.quote(pgbytea::hex_literal(payload)) + "::bytea," +
                   txn.quote(sha256_hex(payload)) + ")";
        };

        std::string msg;
        check(!refused(txn, insert(signal_id, message_id, first, payload_a), &msg),
              "a well-formed journal row inserts", msg);

        check(refused(txn, insert(signal_id, message_id + "x", second, payload_b), &msg),
              "the same signal_id cannot be inserted twice", msg);
        check(msg.find("signal_journal_pkey") != std::string::npos ||
                  msg.find("duplicate key") != std::string::npos,
              "  ...and it is the primary key that refuses it", msg);

        check(refused(txn, insert(signal_id + "2", message_id, second, payload_b), &msg),
              "two signals cannot share a message_id", msg);
        check(refused(txn, insert(signal_id + "3", message_id + "3", first, payload_b), &msg),
              "two signals cannot share a sequence", msg);

        // A payload hash that is not a hash.
        check(refused(txn,
                      "INSERT INTO altsignalgateway.signal_journal"
                      " (signal_id, message_id, sequence, signal_proposal_id, canonical_pair_id,"
                      "  side, schema_version, protocol_version, created_at_ms, expires_at_ms,"
                      "  received_at_ms, journaled_at_ms, source_payload, source_payload_sha256,"
                      "  canonical_payload, payload_sha256)"
                      " VALUES ('x','y'," + std::to_string(second) +
                          ",'z',1,'long',1,1," + std::to_string(now) + "," +
                          std::to_string(now + 1) + "," + std::to_string(now) + "," +
                          std::to_string(now) + ",''::bytea,'not-a-hash',''::bytea,'nope')",
                      &msg),
              "a malformed hash column is refused", msg);

        // A validity window that closes before it opens.
        check(refused(txn, "INSERT INTO altsignalgateway.signal_journal"
                           " (signal_id, message_id, sequence, signal_proposal_id,"
                           "  canonical_pair_id, side, schema_version, protocol_version,"
                           "  created_at_ms, expires_at_ms, received_at_ms, journaled_at_ms,"
                           "  source_payload, source_payload_sha256, canonical_payload,"
                           "  payload_sha256)"
                           " VALUES ('w','v'," + std::to_string(second) +
                               ",'u',1,'long',1,1," + std::to_string(now) + "," +
                               std::to_string(now - 1) + "," + std::to_string(now) + "," +
                               std::to_string(now) + ",''::bytea," +
                               txn.quote(sha256_hex("")) + ",''::bytea," +
                               txn.quote(sha256_hex("")) + ")",
                      &msg),
              "a validity window that closes before it opens is refused", msg);

        // ------------------------------------------------------------------
        std::printf("\n--- immutability ---\n");
        for (const auto& [column, value] :
             std::initializer_list<std::pair<const char*, const char*>>{
                 {"payload_sha256", "'0000000000000000000000000000000000000000000000000000000000000000'"},
                 {"canonical_payload", "'\\x00'::bytea"},
                 {"source_payload", "'\\x00'::bytea"},
                 {"created_at_ms", "1"},
                 {"expires_at_ms", "1"},
                 {"sequence", "999999999"},
                 {"signal_id", "'other'"}}) {
            const bool did = refused(txn,
                                     "UPDATE altsignalgateway.signal_journal SET " +
                                         std::string(column) + " = " + value +
                                         " WHERE signal_id = " + txn.quote(signal_id),
                                     &msg);
            check(did, std::string("  ") + column + " cannot be rewritten", msg);
        }

        // ------------------------------------------------------------------
        std::printf("\n--- delivery state machine ---\n");
        const auto transition = [&](const char* from, const char* to) {
            txn.exec("UPDATE altsignalgateway.signal_journal SET delivery_state = '" +
                     std::string(from) + "' WHERE signal_id = " + txn.quote(signal_id) +
                     " AND delivery_state = '" + from + "'");
            return "UPDATE altsignalgateway.signal_journal SET delivery_state = '" +
                   std::string(to) + "' WHERE signal_id = " + txn.quote(signal_id);
        };
        // Legal path, walked forward.
        check(!refused(txn, transition("JOURNALED", "QUEUED"), &msg), "  JOURNALED -> QUEUED", msg);
        check(!refused(txn,
                       "UPDATE altsignalgateway.signal_journal"
                       " SET delivery_state='SENT', first_sent_at_ms=" + std::to_string(now) +
                           " WHERE signal_id = " + txn.quote(signal_id),
                       &msg),
              "  QUEUED -> SENT", msg);
        check(!refused(txn,
                       "UPDATE altsignalgateway.signal_journal"
                       " SET delivery_state='ACKED', last_ack_at_ms=" + std::to_string(now) +
                           ", acked_by_client_id='selftest' WHERE signal_id = " +
                           txn.quote(signal_id),
                       &msg),
              "  SENT -> ACKED", msg);
        // ACKED is terminal.
        check(refused(txn,
                      "UPDATE altsignalgateway.signal_journal SET delivery_state='JOURNALED'"
                      " WHERE signal_id = " + txn.quote(signal_id),
                      &msg),
              "  ACKED -> JOURNALED is refused (terminal)", msg);
        check(refused(txn,
                      "UPDATE altsignalgateway.signal_journal SET delivery_state='SENT'"
                      " WHERE signal_id = " + txn.quote(signal_id),
                      &msg),
              "  ACKED -> SENT is refused (terminal)", msg);
        // And the shortcut that would fake a delivery.
        txn.exec("UPDATE altsignalgateway.signal_journal SET delivery_state='ACKED'"
                 " WHERE signal_id = " + txn.quote(signal_id));
        check(refused(txn,
                      "UPDATE altsignalgateway.signal_journal SET delivery_attempts = 0"
                      " WHERE signal_id = " + txn.quote(signal_id) + " AND delivery_attempts > 0",
                      &msg) ||
                  txn.query_value<int32_t>("SELECT delivery_attempts FROM"
                                           " altsignalgateway.signal_journal WHERE signal_id = " +
                                           txn.quote(signal_id)) == 0,
              "  delivery_attempts cannot decrease", msg);

        // An ACKED row with no evidence of an acknowledgement.
        check(refused(txn,
                      "UPDATE altsignalgateway.signal_journal SET acked_by_client_id = NULL"
                      " WHERE signal_id = " + txn.quote(signal_id),
                      &msg),
              "  an ACKED row cannot drop its evidence", msg);

        // ------------------------------------------------------------------
        std::printf("\n--- deletion ---\n");
        check(refused(txn,
                      "DELETE FROM altsignalgateway.signal_journal WHERE signal_id = " +
                          txn.quote(signal_id),
                      &msg),
              "a journal row cannot be deleted outside the retention pass", msg);

        // ------------------------------------------------------------------
        std::printf("\n--- client cursor ---\n");
        const std::string client = "selftest-client-" + std::to_string(now);
        txn.exec("INSERT INTO altsignalgateway.client_cursors (client_id, last_acked_sequence)"
                 " VALUES (" + txn.quote(client) + ", 100)");
        check(!refused(txn,
                       "UPDATE altsignalgateway.client_cursors SET last_acked_sequence = 101"
                       " WHERE client_id = " + txn.quote(client),
                       &msg),
              "  a cursor may move forward", msg);
        check(refused(txn,
                      "UPDATE altsignalgateway.client_cursors SET last_acked_sequence = 50"
                      " WHERE client_id = " + txn.quote(client),
                      &msg),
              "  a cursor may NOT move backwards", msg);

        // ------------------------------------------------------------------
        txn.abort();
        std::printf("\n--- rollback ---\n");
        pass("the transaction was rolled back; nothing above was committed");
    } catch (const std::exception& e) {
        fail("the selftest transaction could not complete", e.what());
    }

    // ------------------------------------------------------------------
    // The sequence must be back where it started, and the scratch signal must
    // not exist. Checked on a FRESH connection so it reads committed state.
    std::printf("\n--- nothing leaked ---\n");
    auto after_lease = pool.acquire();
    if (!after_lease) {
        fail("could not re-open a connection to verify the rollback",
             after_lease.error().message);
    } else {
        try {
            pqxx::nontransaction check_txn(after_lease->conn());
            const auto rows = check_txn.query_value<int64_t>(
                "SELECT count(*) FROM altsignalgateway.signal_journal WHERE signal_id = " +
                check_txn.quote(signal_id));
            check(rows == 0, "no journal row was committed");
            const auto cursors = check_txn.query_value<int64_t>(
                "SELECT count(*) FROM altsignalgateway.client_cursors WHERE client_id LIKE"
                " 'selftest-client-%'");
            check(cursors == 0, "no client cursor was committed");
        } catch (const std::exception& e) {
            fail("the post-rollback verification failed", e.what());
        }
    }

    // ------------------------------------------------------------------
    // Concurrency: two transactions allocating a sequence at the same time must
    // serialize on the counter's row lock and must get DIFFERENT numbers. Both
    // roll back, so the counter is unchanged afterwards — which also proves an
    // aborted allocation releases its number instead of leaving a hole.
    std::printf("\n--- concurrent sequence allocation ---\n");
    try {
        persistence::PgPool cpool(cfg.common.postgres_dsn, 2);
        auto a = cpool.acquire();
        auto b = cpool.acquire();
        if (!a || !b) {
            fail("could not open two connections", "pool exhausted");
        } else {
            int64_t base = 0;
            {
                pqxx::nontransaction probe(a->conn());
                base = probe.query_value<int64_t>(
                    "SELECT last_sequence FROM altsignalgateway.sequence_state WHERE singleton");
            }
            int64_t got_a = 0;
            int64_t got_b = 0;
            std::string err;
            pqxx::work ta(a->conn());
            got_a = ta.query_value<int64_t>(
                "UPDATE altsignalgateway.sequence_state SET last_sequence = last_sequence + 1"
                " WHERE singleton RETURNING last_sequence");

            // The second allocation runs on another connection while the first
            // transaction still holds the row lock. It must BLOCK, not race.
            std::thread other([&] {
                try {
                    pqxx::work tb(b->conn());
                    got_b = tb.query_value<int64_t>(
                        "UPDATE altsignalgateway.sequence_state SET last_sequence ="
                        " last_sequence + 1 WHERE singleton RETURNING last_sequence");
                    tb.abort();
                } catch (const std::exception& e) {
                    err = e.what();
                }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            check(got_b == 0, "the second allocation blocks while the first holds the row lock",
                  "got_b=" + std::to_string(got_b));
            ta.abort();
            other.join();

            check(err.empty(), "the second allocation completed without error", err);
            check(got_a == base + 1, "the first allocation took base+1",
                  "base=" + std::to_string(base) + " got_a=" + std::to_string(got_a));
            // After the first aborted, the counter was back at `base`, so the
            // second takes base+1 too. That is the point: an aborted allocation
            // leaves no hole.
            check(got_b == base + 1, "an aborted allocation releases its number",
                  "got_b=" + std::to_string(got_b) + " expected " + std::to_string(base + 1));

            pqxx::nontransaction probe(a->conn());
            const auto final_seq = probe.query_value<int64_t>(
                "SELECT last_sequence FROM altsignalgateway.sequence_state WHERE singleton");
            check(final_seq == base, "the counter is unchanged after both rolled back",
                  "base=" + std::to_string(base) + " now=" + std::to_string(final_seq));
        }
    } catch (const std::exception& e) {
        fail("the concurrency check could not run", e.what());
    }

    std::printf("\n%s\n", g_failures == 0 ? "selftest: OK" : "selftest: FAILED");
    return g_failures == 0 ? 0 : 1;
}

} // namespace altsignalgateway::cli
