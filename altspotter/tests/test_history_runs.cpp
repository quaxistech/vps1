// Ownership of ingestion runs: RunOwner and the statements of history_runs.hpp
// against a real schema, with sessions ended deliberately.
//
// The defect: market_history inserts a run as 'running' and records its result
// when it is done. A process killed in between left the row 'running' until
// retention deleted it seven days later, and bar_aggregation rescanned the
// run's range on every periodic pass. The first fix closed such rows when an
// incarnation took an advisory lock -- but a PostgreSQL restart releases that
// lock under a LIVE process, and a second incarnation would then close the live
// one's runs while it carried on writing them. Ownership is now the session
// itself: run state is written only on the session that holds the lock, so a
// process that lost its session is fenced, and it stops.
//
// Every case here is deterministic: sessions are ended with
// pg_terminate_backend or by closing them, never by waiting for a timeout.

#include "components/agg_boundary.hpp"
#include "components/history_owner.hpp"
#include "components/history_runs.hpp"
#include "test_env.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace altspotter::history;
using altspotter::test::TempDb;

namespace {

constexpr int64_t kMinuteMs = 60'000;

std::string iso(int64_t ms) { return "to_timestamp(" + std::to_string(ms) + "::bigint / 1000.0)"; }

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string text(pqxx::connection& c, const std::string& sql) {
    pqxx::nontransaction tx{c};
    const auto r = tx.exec(sql);
    return r.empty() || r[0][0].is_null() ? "NULL" : r[0][0].as<std::string>();
}

bool owner_alive(pqxx::connection& c) {
    pqxx::nontransaction tx{c};
    return tx.exec_params(kRunOwnerAliveSql, kRunOwnerLockSpace, kRunOwnerLockKey)[0][0].as<bool>();
}

// Sessions holding the run-owner lock in this database: never more than one.
int64_t lock_holders(pqxx::connection& c) {
    return std::stoll(text(c,
        "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND granted AND objsubid = 2"
        " AND database = (SELECT oid FROM pg_database WHERE datname = current_database())"
        " AND classid = " + std::to_string(kRunOwnerLockSpace) + "::oid AND objid = " +
        std::to_string(kRunOwnerLockKey) + "::oid"));
}

// End the owner's session the way a PostgreSQL restart or a network cut ends
// it for the process: server side, while the process itself carries on.
void terminate_owner_session(pqxx::connection& c) {
    const std::string pid = text(c,
        "SELECT pid FROM pg_locks WHERE locktype = 'advisory' AND granted AND objsubid = 2"
        " AND database = (SELECT oid FROM pg_database WHERE datname = current_database())"
        " AND classid = " + std::to_string(kRunOwnerLockSpace) + "::oid AND objid = " +
        std::to_string(kRunOwnerLockKey) + "::oid");
    REQUIRE(pid != "NULL");
    REQUIRE(text(c, "SELECT pg_terminate_backend(" + pid + ")") == "t");
    for (int i = 0; i < 100 && text(c, "SELECT count(*) FROM pg_stat_activity WHERE pid = " + pid) != "0";
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(text(c, "SELECT count(*) FROM pg_stat_activity WHERE pid = " + pid) == "0");
}

// A run started on an owner session, as market_history starts one.
int64_t start_repair(RunOwner& owner, int64_t instrument, int64_t from_ms) {
    int64_t id = -1;
    const bool ok = owner.with_txn("test_repair_start", [&](pqxx::work& txn) {
        id = txn.exec_params(
                    "INSERT INTO ingestion_runs (service, kind, exchange_id, instrument_id,"
                    " timeframe, requested_from, requested_to) VALUES ('market-history-service',"
                    " 'repair', 1, $1, '1m', to_timestamp($2::bigint / 1000.0),"
                    " to_timestamp($3::bigint / 1000.0)) RETURNING id",
                    instrument, from_ms, from_ms + 59 * kMinuteMs)[0][0]
                 .as<int64_t>();
    });
    return ok ? id : -1;
}

// Its result, recorded with the service's own statement. `fenced` is the
// zero-row outcome: the run was closed by a newer owner.
bool finish_repair(RunOwner& owner, int64_t id, bool& fenced) {
    fenced = false;
    return owner.with_txn("test_repair_finish", [&](pqxx::work& txn) {
        fenced = txn.exec_params(kFinishRepairRunSql, id, "completed", "", 60, 60).affected_rows() == 0;
    });
}

} // namespace

TEST_CASE("run ownership is a live session: fenced writes, standby and takeover",
          "[history][orphans][db]") {
    if (!TempDb::admin_dsn()) {
        SKIP("PostgreSQL not available (set ALTSPOTTER_TEST_PG_DSN)");
    }
    TempDb db;
    REQUIRE(db.create());
    const std::string bin = altspotter::test::bin_dir() + "/altspotter";
    REQUIRE(altspotter::test::run_tool(bin, {"migrate", "--dsn", db.dsn(), "--migrations",
                                              altspotter::test::migrations_dir()}) == 0);
    pqxx::connection c{db.dsn()};
    const int64_t t0 = now_ms();
    const auto status_of = [&](int64_t id) {
        return text(c, "SELECT status FROM ingestion_runs WHERE id = " + std::to_string(id));
    };

    SECTION("test 16: a live owner stays the owner; a second incarnation waits and writes nothing") {
        RunOwner a{db.dsn()};
        REQUIRE(a.take() == true);
        const int64_t run = start_repair(a, 10, t0 - 90 * kMinuteMs);
        REQUIRE(run > 0);

        RunOwner b{db.dsn()};
        for (int attempt = 0; attempt < 3; ++attempt) REQUIRE(b.take() == false);   // standby
        REQUIRE_FALSE(b.owner());
        REQUIRE(start_repair(b, 11, t0) == -1);            // a non-owner cannot start a run
        bool fenced = false;
        REQUIRE_FALSE(finish_repair(b, run, fenced));       // nor touch another's
        REQUIRE(status_of(run) == "running");
        REQUIRE(lock_holders(c) == 1);

        REQUIRE(a.holds());
        REQUIRE(finish_repair(a, run, fenced));
        REQUIRE_FALSE(fenced);
        REQUIRE(status_of(run) == "completed");
    }

    SECTION("test 1 (critical): the owner's session is cut while its process lives") {
        RunOwner a{db.dsn()};
        REQUIRE(a.take() == true);
        const int64_t run = start_repair(a, 10, t0 - 90 * kMinuteMs);
        REQUIRE(run > 0);
        RunOwner b{db.dsn()};
        REQUIRE(b.take() == false);   // B is standby while A's session lives

        terminate_owner_session(c);   // PostgreSQL restart / cut, as A sees it

        // A is fenced before it even notices: its write fails and changes nothing.
        bool fenced = false;
        REQUIRE_FALSE(finish_repair(a, run, fenced));
        REQUIRE(a.lost());
        REQUIRE(status_of(run) == "running");
        // It noticed, and it never takes the lock back on its own.
        REQUIRE_FALSE(a.holds());
        REQUIRE(a.take() == false);

        // B takes over: the lock is free, A's run was written by a session that
        // no longer exists, and B closes it.
        REQUIRE(b.take() == true);
        REQUIRE(lock_holders(c) == 1);
        const auto closed = b.close_orphans();
        REQUIRE(closed.has_value());
        REQUIRE(closed->size() == 1);
        REQUIRE(closed->front().id == run);
        REQUIRE(status_of(run) == "aborted");

        // A can never overwrite B's verdict, however long its process lives.
        REQUIRE_FALSE(finish_repair(a, run, fenced));
        REQUIRE(status_of(run) == "aborted");
        REQUIRE(start_repair(a, 10, t0) == -1);
        REQUIRE(lock_holders(c) == 1);
    }

    SECTION("test 1b: the cut owner is fenced even if it tries to take the lock first") {
        RunOwner a{db.dsn()};
        REQUIRE(a.take() == true);
        const int64_t run = start_repair(a, 10, t0 - 90 * kMinuteMs);
        terminate_owner_session(c);
        REQUIRE_FALSE(a.holds());
        // Nobody else is there, the lock is free -- and the lost incarnation
        // still does not take it: only a fresh incarnation competes.
        REQUIRE(a.take() == false);
        REQUIRE(lock_holders(c) == 0);
        // A fresh incarnation of the same process (the supervisor's restart)
        // becomes the owner and closes the run the old one could not finish.
        RunOwner a2{db.dsn()};
        REQUIRE(a2.take() == true);
        const auto closed = a2.close_orphans();
        REQUIRE(closed.has_value());
        REQUIRE(closed->size() == 1);
        REQUIRE(status_of(run) == "aborted");
    }

    SECTION("test 14 (model): an owner process that dies releases the lock with its session") {
        auto a = std::make_unique<RunOwner>(db.dsn());
        REQUIRE(a->take() == true);
        const int64_t run = start_repair(*a, 10, t0 - 90 * kMinuteMs);
        RunOwner b{db.dsn()};
        REQUIRE(b.take() == false);
        a.reset();   // the process is gone; its sockets close
        bool taken = false;
        for (int i = 0; i < 100 && !taken; ++i) {
            taken = b.take() == true;
            if (!taken) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        REQUIRE(taken);
        REQUIRE(b.close_orphans()->size() == 1);
        REQUIRE(status_of(run) == "aborted");
    }

    SECTION("test 15: power-loss model -- rows committed, no session anywhere") {
        {
            pqxx::work tx{c};
            tx.exec("INSERT INTO ingestion_runs (id, service, kind, exchange_id, instrument_id,"
                    " requested_from, requested_to, status, started_at) VALUES"
                    " (1, 'market-history-service', 'repair', 1, 10, " + iso(t0 - 90 * kMinuteMs) +
                    ", " + iso(t0 - 31 * kMinuteMs) + ", 'running', " + iso(t0 - 3 * kMinuteMs) + "),"
                    " (2, 'market-history-service', 'bootstrap', 1, 11, " +
                    iso(t0 - 180LL * 86'400'000) + ", " + iso(t0 - 120 * kMinuteMs) +
                    ", 'running', " + iso(t0 - 120 * kMinuteMs) + ")");
            tx.exec("INSERT INTO ingestion_runs (id, service, kind, exchange_id, instrument_id,"
                    " requested_from, requested_to, status, started_at, finished_at) VALUES"
                    " (3, 'market-history-service', 'repair', 1, 10, " + iso(t0 - 300 * kMinuteMs) +
                    ", " + iso(t0 - 250 * kMinuteMs) + ", 'completed', " + iso(t0 - 70 * kMinuteMs) +
                    ", " + iso(t0 - 60 * kMinuteMs) + ")");
            tx.exec("INSERT INTO history_coverage (exchange_id, instrument_id, timeframe, status,"
                    " requested_from, updated_at) VALUES"
                    " (1, 11, '1m', 'bootstrapping', " + iso(t0 - 180LL * 86'400'000) + ", now()),"
                    " (1, 12, '1m', 'ready', " + iso(t0 - 180LL * 86'400'000) + ", now())");
            tx.exec("SELECT setval(pg_get_serial_sequence('ingestion_runs', 'id'), 100)");
            tx.commit();
        }
        REQUIRE_FALSE(owner_alive(c));
        RunOwner next{db.dsn()};
        REQUIRE(next.take() == true);
        const auto closed = next.close_orphans();
        REQUIRE(closed.has_value());
        REQUIRE(closed->size() == 2);
        REQUIRE(closed->at(0).id == 1);
        REQUIRE_FALSE(closed->at(0).bootstrap_resumed);
        REQUIRE(closed->at(1).id == 2);
        REQUIRE(closed->at(1).bootstrap_resumed);
        // The three-minute-old one too: young, but no session is left to own it.
        REQUIRE(text(c, "SELECT count(*) FROM ingestion_runs WHERE id IN (1, 2)"
                        " AND status = 'aborted' AND finished_at IS NOT NULL"
                        " AND error LIKE 'orphaned:%'") == "2");
        REQUIRE(text(c, "SELECT status || ' ' || (extract(epoch FROM finished_at) * 1000)::bigint"
                        " FROM ingestion_runs WHERE id = 3") ==
                "completed " + std::to_string(t0 - 60 * kMinuteMs));
        REQUIRE(text(c, "SELECT status FROM history_coverage WHERE instrument_id = 11") == "pending");
        REQUIRE(text(c, "SELECT status FROM history_coverage WHERE instrument_id = 12") == "ready");
        REQUIRE(next.close_orphans()->empty());   // idempotent
    }

    SECTION("crash window B: the start was never committed -- nothing to recover") {
        auto a = std::make_unique<RunOwner>(db.dsn());
        REQUIRE(a->take() == true);
        // The process dies inside the start transaction: the session ends with
        // it and PostgreSQL rolls the transaction back.
        const bool committed = a->with_txn("test_start_then_die", [&](pqxx::work& txn) {
            txn.exec("INSERT INTO ingestion_runs (service, kind, exchange_id, instrument_id,"
                     " timeframe, requested_from, requested_to) VALUES ('market-history-service',"
                     " 'bootstrap', 1, 13, '1m', now() - interval '1 day', now())");
            txn.exec("INSERT INTO history_coverage (exchange_id, instrument_id, timeframe,"
                     " requested_from, status) VALUES (1, 13, '1m', now() - interval '1 day',"
                     " 'bootstrapping')");
            throw std::runtime_error("process died before commit");
        });
        REQUIRE_FALSE(committed);
        a.reset();
        REQUIRE(text(c, "SELECT count(*) FROM ingestion_runs WHERE instrument_id = 13") == "0");
        REQUIRE(text(c, "SELECT count(*) FROM history_coverage WHERE instrument_id = 13") == "0");
        RunOwner next{db.dsn()};
        bool taken = false;
        for (int i = 0; i < 100 && !taken; ++i) {
            taken = next.take() == true;
            if (!taken) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        REQUIRE(taken);
        REQUIRE(next.close_orphans()->empty());   // no false orphan
    }

    SECTION("test 17 (reader): a closed orphan is rescanned once more, then drops out") {
        RunOwner a{db.dsn()};
        REQUIRE(a.take() == true);
        const int64_t run = start_repair(a, 10, t0 - 90 * kMinuteMs);
        terminate_owner_session(c);
        RunOwner b{db.dsn()};
        REQUIRE(b.take() == true);
        REQUIRE(b.close_orphans()->size() == 1);
        const auto runs = [&](int64_t committed_through, bool alive) {
            pqxx::work tx{c};
            return tx.exec_params(altspotter::aggregation::kBackfillRunsSql, 1, 10,
                                  committed_through, alive)
                .size();
        };
        // Its rows may have been committed after the previous pass, so the next
        // pass rescans its range even though it is no longer 'running'...
        REQUIRE(runs(t0 - 10 * kMinuteMs, true) == 1);
        // ...and once the writer frontier has passed its close by the horizon,
        // it never widens a pass again.
        REQUIRE(runs(now_ms() + 5 * kMinuteMs, true) == 0);
        REQUIRE(status_of(run) == "aborted");
    }
}
