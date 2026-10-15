// Publication after archive (BUG-01 / BUG-02), against a real PostgreSQL 16
// and a real JetStream: a decision is archived whole or not at all, nothing
// reaches the stream that the archive did not commit, only the stream's ack
// marks a message sent, a lost ack is a duplicate rather than a second
// message, and an outage of either side loses nothing.
//
// Needs PGBIN (the PostgreSQL 16 server binaries) and nats-server on PATH;
// without them every case SKIPs. The cluster and the JetStream store live in a
// fresh temporary directory, are owned by this process and are removed at exit
// -- never a shared or production database.

#include "altstrategy/engine/outcomes.hpp"
#include "altstrategy/engine_io/decision_engine.hpp"
#include "altstrategy/engine_io/decision_store.hpp"
#include "altstrategy/engine_io/maintenance_scheduler.hpp"
#include "altstrategy/engine_io/market_reader.hpp"
#include "altstrategy/engine_io/outbox_publisher.hpp"
#include "altstrategy/engine_io/snapshot_exporter.hpp"
#include "altstrategy/messaging/nats_client.hpp"
#include "altstrategy/persistence/pg.hpp"
#include "engine_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <nats.h>
#include <pqxx/pqxx>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <map>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

using namespace altstrategy;
using namespace altstrategy::engine;
using namespace altstrategy::engine_io;
namespace fs = std::filesystem;

namespace {

int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

bool on_path(const std::string& exe) {
    const char* path = std::getenv("PATH");
    if (path == nullptr) return false;
    std::string p(path);
    for (size_t start = 0;;) {
        const size_t end = p.find(':', start);
        if (fs::exists(fs::path(p.substr(start, end - start)) / exe)) return true;
        if (end == std::string::npos) return false;
        start = end + 1;
    }
}

// The scratch PostgreSQL cluster and nats-server, created once per process.
class Env {
public:
    Env() {
        const char* pgbin = std::getenv("PGBIN");
        if (pgbin == nullptr || !fs::exists(fs::path(pgbin) / "postgres")) { why_ = "PGBIN not set"; return; }
        if (!on_path("nats-server")) { why_ = "nats-server not on PATH"; return; }
        pgbin_ = pgbin;
        char tmpl[] = "/tmp/altstrategy-outbox-it-XXXXXX";
        if (mkdtemp(tmpl) == nullptr) { why_ = "mkdtemp failed"; return; }
        dir_ = tmpl;
        pg_port_ = 21000 + static_cast<int>(getpid() % 4000);
        nats_port_ = pg_port_ + 4000;
        if (!sh("'" + pgbin_ + "/initdb' -D '" + (dir_ / "pg").string() +
                "' -A trust -U postgres --no-sync >'" + (dir_ / "initdb.log").string() + "' 2>&1") ||
            !start_pg()) { why_ = "could not start PostgreSQL"; return; }
        if (!admin_sql("postgres", "CREATE ROLE altstrategy_app LOGIN; "
                                   "CREATE DATABASE altstrategy OWNER altstrategy_app;")) {
            why_ = "could not create the database"; return;
        }
        std::vector<fs::path> migrations;
        for (const auto& e : fs::directory_iterator(fs::path(ALTSTRATEGY_SOURCE_DIR) / "db" / "migrations"))
            migrations.push_back(e.path());
        std::sort(migrations.begin(), migrations.end());
        for (const auto& m : migrations) {
            // As the service role, exactly as altstrategy-db-migrate runs them.
            if (!sh("PGOPTIONS='-c role=altstrategy_app' '" + pgbin_ + "/psql' -X -q -v ON_ERROR_STOP=1 -1 -h 127.0.0.1 -p " +
                    std::to_string(pg_port_) + " -U postgres -d altstrategy -f '" + m.string() + "' >/dev/null")) {
                why_ = "migration failed: " + m.filename().string(); return;
            }
        }
        if (!start_nats()) { why_ = "could not start nats-server"; return; }
        ok_ = true;
    }
    ~Env() {
        stop_nats();
        if (!dir_.empty()) {
            sh("'" + pgbin_ + "/pg_ctl' -D '" + (dir_ / "pg").string() + "' -m immediate stop >/dev/null 2>&1");
            std::error_code ec;
            fs::remove_all(dir_, ec);
        }
    }

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] const std::string& why() const { return why_; }
    [[nodiscard]] std::string dsn() const {
        return "host=127.0.0.1 port=" + std::to_string(pg_port_) +
               " dbname=altstrategy user=altstrategy_app connect_timeout=2";
    }
    [[nodiscard]] std::string admin_dsn() const {
        return "host=127.0.0.1 port=" + std::to_string(pg_port_) + " dbname=altstrategy user=postgres";
    }
    [[nodiscard]] std::string nats_url() const { return "nats://127.0.0.1:" + std::to_string(nats_port_); }

    bool start_pg() {
        return sh("'" + pgbin_ + "/pg_ctl' -D '" + (dir_ / "pg").string() + "' -w -l '" +
                  (dir_ / "pg.log").string() + "' -o \"-p " + std::to_string(pg_port_) + " -k '" +
                  dir_.string() + "' -c listen_addresses=127.0.0.1 -c fsync=off\" start >/dev/null");
    }
    void stop_pg() {
        sh("'" + pgbin_ + "/pg_ctl' -D '" + (dir_ / "pg").string() + "' -m immediate stop >/dev/null 2>&1");
    }
    bool start_nats() {
        const std::string port = std::to_string(nats_port_);
        const std::string store = (dir_ / "js").string();
        std::vector<std::string> args = {"nats-server", "-a", "127.0.0.1", "-p", port, "-js", "-sd", store};
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        const std::string log = (dir_ / "nats.log").string();
        posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        posix_spawn_file_actions_addopen(&fa, 2, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        const int rc = posix_spawnp(&nats_pid_, "nats-server", &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0) return false;
        for (int i = 0; i < 100; ++i) {   // until it accepts connections
            if (sh("bash -c '</dev/tcp/127.0.0.1/" + port + "' 2>/dev/null")) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }
    void stop_nats() {
        if (nats_pid_ <= 0) return;
        kill(nats_pid_, SIGTERM);
        int status = 0;
        waitpid(nats_pid_, &status, 0);
        nats_pid_ = -1;
    }

    bool admin_sql(const std::string& db, const std::string& sql) {
        const fs::path f = dir_ / "admin.sql";
        { std::ofstream(f) << sql; }
        return sh("'" + pgbin_ + "/psql' -X -q -v ON_ERROR_STOP=1 -h 127.0.0.1 -p " + std::to_string(pg_port_) +
                  " -U postgres -d " + db + " -f '" + f.string() + "' >/dev/null");
    }
    // One of the repository's operator files, run as the service role -- the
    // way its header says to run it.
    bool operator_sql(const fs::path& relative) {
        return sh("PGOPTIONS='-c role=altstrategy_app' '" + pgbin_ + "/psql' -X -q -v ON_ERROR_STOP=1 -h 127.0.0.1 -p " +
                  std::to_string(pg_port_) + " -U postgres -d altstrategy -f '" +
                  (fs::path(ALTSTRATEGY_SOURCE_DIR) / relative).string() + "' >/dev/null");
    }
    int64_t count(const std::string& sql) {
        pqxx::connection c(admin_dsn());
        pqxx::work w(c);
        return w.exec1(sql)[0].as<int64_t>();
    }

private:
    static bool sh(const std::string& cmd) { return std::system(cmd.c_str()) == 0; }

    bool ok_ = false;
    std::string why_;
    std::string pgbin_;
    fs::path dir_;
    int pg_port_ = 0, nats_port_ = 0;
    pid_t nats_pid_ = -1;
};

Env& env() {
    static Env e;
    return e;
}

#define REQUIRE_ENV() \
    do { if (!env().ok()) SKIP("integration environment unavailable: " << env().why()); } while (0)

std::unique_ptr<messaging::NatsClient> connect_nats() {
    auto c = messaging::NatsClient::connect(env().nats_url(), "altstrategy-outbox-it");
    REQUIRE(c.has_value());
    return std::move(*c);
}

// The stream AltStrategy owns, as altstrategy-nats-init provisions it.
void ensure_stream(messaging::NatsClient& nats) {
    static const char* subjects[] = {"strategy.>", "tradeplan.>", "tradeproposal.>", "signal.>",
                                     "archive.snapshot.>"};
    jsStreamConfig cfg;
    jsStreamConfig_Init(&cfg);
    cfg.Name = "ALTSTRATEGY_DECISIONS";
    cfg.Subjects = subjects;
    cfg.SubjectsLen = 5;
    cfg.Storage = js_FileStorage;
    cfg.Duplicates = 24LL * 3600 * 1000 * 1000 * 1000;
    jsStreamInfo* si = nullptr;
    jsErrCode jerr = static_cast<jsErrCode>(0);
    if (js_GetStreamInfo(&si, nats.js(), "ALTSTRATEGY_DECISIONS", nullptr, &jerr) != NATS_OK) {
        REQUIRE(js_AddStream(&si, nats.js(), &cfg, nullptr, &jerr) == NATS_OK);
    }
    if (si != nullptr) jsStreamInfo_Destroy(si);
}

uint64_t stream_messages(messaging::NatsClient& nats) {
    jsStreamInfo* si = nullptr;
    jsErrCode jerr = static_cast<jsErrCode>(0);
    REQUIRE(js_GetStreamInfo(&si, nats.js(), "ALTSTRATEGY_DECISIONS", nullptr, &jerr) == NATS_OK);
    const uint64_t n = si->State.Msgs;
    jsStreamInfo_Destroy(si);
    return n;
}

// The stored message at `seq`: its Nats-Msg-Id and payload.
std::pair<std::string, std::string> stream_message(messaging::NatsClient& nats, uint64_t seq) {
    natsMsg* m = nullptr;
    jsErrCode jerr = static_cast<jsErrCode>(0);
    REQUIRE(js_GetMsg(&m, nats.js(), "ALTSTRATEGY_DECISIONS", seq, nullptr, &jerr) == NATS_OK);
    const char* id = nullptr;
    natsMsgHeader_Get(m, "Nats-Msg-Id", &id);
    std::pair<std::string, std::string> out{id ? id : "",
                                            std::string(natsMsg_GetData(m),
                                                        static_cast<size_t>(natsMsg_GetDataLength(m)))};
    natsMsg_Destroy(m);
    return out;
}

// A context the catalogue raises candidates on (make_ctx), stamped now and
// unique per scenario so its candidate ids are fresh.
DecisionContext live_ctx(int64_t pair, int64_t rev = 0) {
    auto ctx = test::make_ctx(100.0, "TREND_UP");
    const int64_t now = wall_ms();
    ctx.canonical_pair_id = pair;
    ctx.bitget_instrument_id = pair;
    ctx.context_id = "ctx-it-" + std::to_string(pair) + "-" + std::to_string(rev);
    ctx.context_revision = pair * 100 + rev;
    ctx.event_time_ms = now;
    ctx.available_at_ms = now;
    ctx.expires_at_ms = now + 3600'000;
    ctx.symbol = "IT" + std::to_string(pair) + "USDT";
    return ctx;
}

int64_t rows_for_pair(const std::string& table, int64_t pair) {
    return env().count("SELECT count(*) FROM altstrategy." + table + " WHERE canonical_pair_id = " +
                       std::to_string(pair));
}

int64_t outbox_rows(int64_t pair, const std::string& extra = "") {
    return env().count("SELECT count(*) FROM altstrategy.publication_outbox o "
                       "JOIN altstrategy.trade_signal_proposals p ON p.signal_proposal_id = o.signal_proposal_id "
                       "WHERE p.canonical_pair_id = " + std::to_string(pair) + extra);
}

} // namespace

TEST_CASE("a decision is archived with its publications in one commit, and only then sent",
          "[integration][outbox]") {
    REQUIRE_ENV();
    auto nats = connect_nats();
    ensure_stream(*nats);
    persistence::PgPool pool(env().dsn(), 4);
    DecisionStore store(pool);
    OutboxPublisher publisher(store, *nats);
    DecisionEngine engine(*nats, store, publisher, DecisionEngineConfig{});

    const int64_t pair = 9001;
    engine.run_for_context(live_ctx(pair));
    const int64_t candidates = rows_for_pair("strategy_candidates", pair);
    const int64_t proposals = rows_for_pair("trade_signal_proposals", pair);
    REQUIRE(candidates > 0);
    CHECK(rows_for_pair("trade_plan_variants", pair) == 3 * candidates);
    CHECK(proposals == 3 * candidates);
    CHECK(env().count("SELECT count(*) FROM altstrategy.decision_context_snapshots WHERE canonical_pair_id = 9001") == 1);
    // One trade_proposal per publishable variant, one signal_proposed per main one.
    const int64_t publishable = env().count(
        "SELECT count(*) FROM altstrategy.trade_signal_proposals WHERE canonical_pair_id = 9001 "
        "AND proposal_grade <> 'INVALID'");
    const int64_t main_publishable = env().count(
        "SELECT count(*) FROM altstrategy.trade_signal_proposals WHERE canonical_pair_id = 9001 "
        "AND proposal_grade <> 'INVALID' AND plan_version = 1");
    CHECK(outbox_rows(pair) == publishable + main_publishable);
    CHECK(outbox_rows(pair, " AND o.settled_at IS NULL") == publishable + main_publishable);
    CHECK(env().count("SELECT count(*) FROM altstrategy.trade_signal_proposals WHERE canonical_pair_id = 9001 "
                      "AND plan_geometry_version = 2") == proposals);

    // Sent with its Nats-Msg-Id, marked only on the stream's ack.
    const uint64_t before = stream_messages(*nats);
    const int acked = publisher.drain_once(wall_ms());
    CHECK(acked == publishable + main_publishable);
    CHECK(stream_messages(*nats) == before + static_cast<uint64_t>(acked));
    CHECK(outbox_rows(pair, " AND o.settled_at IS NULL") == 0);
    CHECK(outbox_rows(pair, " AND o.stream_sequence > 0 AND o.published_at_ms IS NOT NULL") == acked);
    pqxx::connection admin(env().admin_dsn());
    {
        pqxx::work w(admin);
        const auto row = w.exec1(
            "SELECT o.message_id, o.stream_sequence, o.payload FROM altstrategy.publication_outbox o "
            "JOIN altstrategy.trade_signal_proposals p ON p.signal_proposal_id = o.signal_proposal_id "
            "WHERE p.canonical_pair_id = 9001 AND o.kind = 'signal_proposed' LIMIT 1");
        const auto [id, payload] = stream_message(*nats, row[1].as<uint64_t>());
        const auto bytes = row[2].as<std::basic_string<std::byte>>();
        CHECK(id == row[0].as<std::string>());
        CHECK(payload == std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    // Every message sent names an archive that exists: its proposal, the
    // candidate, the context that candidate was decided on, and the ranking.
    CHECK(env().count(
              "SELECT count(*) FROM altstrategy.publication_outbox o "
              "JOIN altstrategy.trade_signal_proposals p ON p.signal_proposal_id = o.signal_proposal_id "
              "LEFT JOIN altstrategy.strategy_candidates c ON c.candidate_id = p.candidate_id "
              "LEFT JOIN altstrategy.decision_context_snapshots x ON x.context_id = c.context_id "
              "LEFT JOIN altstrategy.strategy_rankings r ON r.ranking_id = p.ranking_id "
              "WHERE p.canonical_pair_id = 9001 "
              "AND (c.candidate_id IS NULL OR x.context_id IS NULL OR r.ranking_id IS NULL)") == 0);

    // A re-send after an ack the process never recorded is a duplicate the
    // stream drops -- never a second message.
    {
        pqxx::work w(admin);
        w.exec("UPDATE altstrategy.publication_outbox SET published_at_ms = NULL, stream_sequence = NULL, "
               "settled_at = NULL WHERE message_id IN (SELECT message_id FROM altstrategy.publication_outbox "
               "WHERE kind = 'signal_proposed' ORDER BY created_at_ms LIMIT 1)");
        w.commit();
    }
    const uint64_t settled = stream_messages(*nats);
    CHECK(publisher.drain_once(wall_ms()) == 1);
    CHECK(stream_messages(*nats) == settled);
}

TEST_CASE("a decision whose archive fails is published nowhere", "[integration][outbox]") {
    REQUIRE_ENV();
    auto nats = connect_nats();
    ensure_stream(*nats);
    persistence::PgPool pool(env().dsn(), 4);
    DecisionStore store(pool);
    OutboxPublisher publisher(store, *nats);
    DecisionEngine engine(*nats, store, publisher, DecisionEngineConfig{});

    // The proposal insert fails after the candidate and its plan variants are
    // already written in the same transaction: all of it must roll back.
    REQUIRE(env().admin_sql("altstrategy",
        "CREATE FUNCTION altstrategy.it_poison() RETURNS trigger LANGUAGE plpgsql AS "
        "$$ BEGIN RAISE EXCEPTION 'injected archive failure'; END $$; "
        "CREATE TRIGGER it_poison BEFORE INSERT ON altstrategy.trade_signal_proposals FOR EACH ROW "
        "WHEN (NEW.canonical_pair_id = 9002) EXECUTE FUNCTION altstrategy.it_poison();"));
    const int64_t pair = 9002;
    const uint64_t before = stream_messages(*nats);
    engine.run_for_context(live_ctx(pair));
    REQUIRE(env().admin_sql("altstrategy", "DROP TRIGGER it_poison ON altstrategy.trade_signal_proposals; "
                                           "DROP FUNCTION altstrategy.it_poison();"));
    CHECK(rows_for_pair("strategy_candidates", pair) == 0);
    CHECK(rows_for_pair("trade_plan_variants", pair) == 0);
    CHECK(rows_for_pair("trade_signal_proposals", pair) == 0);
    CHECK(publisher.drain_once(wall_ms()) == 0);
    CHECK(stream_messages(*nats) == before);

    // A context that cannot be archived produces no decision at all.
    REQUIRE(env().admin_sql("altstrategy",
        "REVOKE INSERT ON altstrategy.decision_context_snapshots FROM altstrategy_app;"));
    engine.run_for_context(live_ctx(9003));
    REQUIRE(env().admin_sql("altstrategy",
        "GRANT INSERT ON altstrategy.decision_context_snapshots TO altstrategy_app;"));
    CHECK(rows_for_pair("strategy_candidates", 9003) == 0);
    CHECK(rows_for_pair("trade_signal_proposals", 9003) == 0);
}

TEST_CASE("a ranking that cannot be archived is not named by the proposals it ranked",
          "[integration][outbox]") {
    REQUIRE_ENV();
    auto nats = connect_nats();
    ensure_stream(*nats);
    persistence::PgPool pool(env().dsn(), 4);
    DecisionStore store(pool);
    OutboxPublisher publisher(store, *nats);
    DecisionEngine engine(*nats, store, publisher, DecisionEngineConfig{});

    const int64_t pair = 9010;
    REQUIRE(env().admin_sql("altstrategy",
        "ALTER TABLE altstrategy.strategy_rankings ADD CONSTRAINT it_no_ranking "
        "CHECK (canonical_pair_id <> 9010) NOT VALID;"));
    engine.run_for_context(live_ctx(pair));
    REQUIRE(env().admin_sql("altstrategy",
        "ALTER TABLE altstrategy.strategy_rankings DROP CONSTRAINT it_no_ranking;"));
    CHECK(rows_for_pair("strategy_rankings", pair) == 0);
    // Rankings gate nothing: the decisions are archived and owed, naming none.
    REQUIRE(rows_for_pair("trade_signal_proposals", pair) > 0);
    CHECK(env().count("SELECT count(*) FROM altstrategy.trade_signal_proposals WHERE canonical_pair_id = 9010 "
                      "AND COALESCE(ranking_id, '') <> ''") == 0);
    REQUIRE(outbox_rows(pair, " AND o.settled_at IS NULL") > 0);
    publisher.drain_once(wall_ms());
    CHECK(outbox_rows(pair, " AND o.settled_at IS NULL") == 0);
}

TEST_CASE("re-deciding a context reproduces its identifiers and owes nothing twice", "[integration][outbox]") {
    REQUIRE_ENV();
    auto nats = connect_nats();
    ensure_stream(*nats);
    persistence::PgPool pool(env().dsn(), 4);
    DecisionStore store(pool);
    OutboxPublisher publisher(store, *nats);

    const int64_t pair = 9011;
    const auto ctx = live_ctx(pair);
    DecisionEngine first(*nats, store, publisher, DecisionEngineConfig{});
    first.run_for_context(ctx);
    const int64_t proposals = rows_for_pair("trade_signal_proposals", pair);
    const int64_t owed = outbox_rows(pair);
    REQUIRE(owed > 0);
    publisher.drain_once(wall_ms());
    REQUIRE(outbox_rows(pair, " AND o.settled_at IS NULL") == 0);

    // A redelivered input, a replay, a second process: the same context decided
    // again by an engine with no memory of the first time.
    DecisionEngine again(*nats, store, publisher, DecisionEngineConfig{});
    again.run_for_context(ctx);
    CHECK(rows_for_pair("trade_signal_proposals", pair) == proposals);
    CHECK(outbox_rows(pair) == owed);
    // Nothing is owed again, so nothing is sent again. (Candidates, rankings and
    // ensembles are best-effort Core publishes without an id: those go out
    // again -- observability on strategy.*, not signals.)
    CHECK(outbox_rows(pair, " AND o.settled_at IS NULL") == 0);
    CHECK(publisher.drain_once(wall_ms()) == 0);
}

TEST_CASE("an unavailable stream leaves the messages owed until it is back", "[integration][outbox]") {
    REQUIRE_ENV();
    auto nats = connect_nats();
    ensure_stream(*nats);
    persistence::PgPool pool(env().dsn(), 4);
    DecisionStore store(pool);
    OutboxPublisherConfig pcfg;
    pcfg.ack_timeout_ms = 1'000;
    OutboxPublisher publisher(store, *nats, pcfg);
    DecisionEngine engine(*nats, store, publisher, DecisionEngineConfig{});

    const int64_t pair = 9004;
    engine.run_for_context(live_ctx(pair));
    const int64_t owed = outbox_rows(pair, " AND o.settled_at IS NULL");
    REQUIRE(owed > 0);

    env().stop_nats();
    const int64_t t0 = wall_ms();
    CHECK(publisher.drain_once(t0) == 0);
    // One attempt recorded on the first row; it ended the pass (transport failure).
    CHECK(outbox_rows(pair, " AND o.settled_at IS NULL") == owed);
    CHECK(env().count("SELECT count(*) FROM altstrategy.publication_outbox WHERE attempts = 1 AND "
                      "last_error IS NOT NULL AND settled_at IS NULL AND next_attempt_at_ms > " +
                      std::to_string(t0)) >= 1);

    REQUIRE(env().start_nats());
    for (int i = 0; i < 100 && !nats->connected(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(nats->connected());
    // A process restarted after the outage resumes from the table.
    OutboxPublisher restarted(store, *nats, pcfg);
    const int64_t later = wall_ms() + pcfg.retry_cap_ms;
    int sent = 0;
    for (int pass = 0; pass < 5 && sent < owed; ++pass) sent += restarted.drain_once(later);
    CHECK(sent >= owed);
    CHECK(outbox_rows(pair, " AND o.settled_at IS NULL") == 0);
}

TEST_CASE("an undeliverable message is dead-lettered, kept, and sent once requeued; retention drops neither",
          "[integration][outbox]") {
    REQUIRE_ENV();
    auto nats = connect_nats();
    persistence::PgPool pool(env().dsn(), 4);
    DecisionStore store(pool);
    const int64_t now = wall_ms();
    // A market outcome whose subject no stream captures: JetStream answers
    // "no responders" every time.
    MarketOutcome o;
    o.outcome_id = "mkt_it_deadletter";
    o.candidate_id = "cand_it_deadletter";
    o.is_counterfactual = true;
    o.outcome = MarketOutcomeType::time_stop;
    o.created_at_ms = now;
    const OutboxMessage msg{"market_outcome:mkt_it_deadletter", "market_outcome", "it.nostream.outcome",
                            "payload", "", "cand_it_deadletter", now - 2 * 3600'000};
    REQUIRE(store.save_market_outcome(o, &msg));
    OutboxPublisherConfig pcfg;
    pcfg.ack_timeout_ms = 500;
    pcfg.max_age_ms = 3600'000;
    OutboxPublisher publisher(store, *nats, pcfg);
    CHECK(publisher.drain_once(now) == 0);
    const std::string dead = "message_id = 'market_outcome:mkt_it_deadletter'";
    CHECK(env().count("SELECT count(*) FROM altstrategy.publication_outbox WHERE " + dead +
                      " AND dead_lettered_at_ms IS NOT NULL AND settled_at IS NULL AND published_at_ms IS NULL "
                      "AND last_error IS NOT NULL") == 1);
    // Out of the publisher's scan, and counted where an operator looks.
    CHECK(publisher.drain_once(now + 3600'000) == 0);
    const auto backlog = store.outbox_backlog();
    REQUIRE(backlog.has_value());
    CHECK(backlog->dead_lettered >= 1);

    // Retention purges on settled_at, so neither a row still owed nor one given
    // up on is ever reclaimed, however old.
    REQUIRE(env().admin_sql("altstrategy",
        "INSERT INTO altstrategy.publication_outbox(message_id,kind,subject,payload,created_at_ms,"
        "next_attempt_at_ms,created_at) VALUES ('market_outcome:it_old_owed','market_outcome','x','\\x00'::bytea,"
        "0, 9000000000000, now() - interval '400 days');"
        "UPDATE altstrategy.publication_outbox SET created_at = now() - interval '400 days' WHERE " + dead + ";"));
    store.run_retention(now);
    CHECK(env().count("SELECT count(*) FROM altstrategy.publication_outbox WHERE message_id = "
                      "'market_outcome:it_old_owed'") == 1);
    CHECK(env().count("SELECT count(*) FROM altstrategy.publication_outbox WHERE " + dead) == 1);

    // The cause fixed (a stream now captures the subject), the operator's
    // requeue sends it under its original id.
    {
        static const char* subjects[] = {"it.nostream.>"};
        jsStreamConfig cfg;
        jsStreamConfig_Init(&cfg);
        cfg.Name = "IT_RECOVERY";
        cfg.Subjects = subjects;
        cfg.SubjectsLen = 1;
        cfg.Storage = js_FileStorage;
        jsStreamInfo* si = nullptr;
        jsErrCode jerr = static_cast<jsErrCode>(0);
        REQUIRE(js_AddStream(&si, nats->js(), &cfg, nullptr, &jerr) == NATS_OK);
        jsStreamInfo_Destroy(si);
    }
    REQUIRE(env().operator_sql("db/operator/outbox_requeue_dead_lettered.sql"));
    CHECK(publisher.drain_once(wall_ms()) >= 1);
    CHECK(env().count("SELECT count(*) FROM altstrategy.publication_outbox WHERE " + dead +
                      " AND published_at_ms IS NOT NULL AND settled_at IS NOT NULL AND stream_sequence > 0 "
                      "AND dead_lettered_at_ms IS NULL") == 1);
}

TEST_CASE("a database outage refuses the archive and publishes nothing", "[integration][outbox]") {
    REQUIRE_ENV();
    auto nats = connect_nats();
    ensure_stream(*nats);
    persistence::PgPool pool(env().dsn(), 4);
    DecisionStore store(pool);
    OutboxPublisher publisher(store, *nats);
    DecisionEngine engine(*nats, store, publisher, DecisionEngineConfig{});
    const uint64_t before = stream_messages(*nats);

    env().stop_pg();
    engine.run_for_context(live_ctx(9005));
    CHECK(publisher.drain_once(wall_ms()) == 0);   // the backlog read fails: no publish
    CHECK(stream_messages(*nats) == before);
    REQUIRE(env().start_pg());

    // Back: the next decision archives and goes out normally.
    engine.run_for_context(live_ctx(9006));
    CHECK(rows_for_pair("trade_signal_proposals", 9005) == 0);
    CHECK(rows_for_pair("trade_signal_proposals", 9006) > 0);
    CHECK(publisher.drain_once(wall_ms()) > 0);
}

TEST_CASE("a restart does not re-issue the hypotheses still open", "[integration][design-04]") {
    REQUIRE_ENV();
    auto nats = connect_nats();
    ensure_stream(*nats);
    persistence::PgPool pool(env().dsn(), 4);
    DecisionStore store(pool);
    OutboxPublisher publisher(store, *nats);
    const int64_t pair = 9201;
    DecisionEngine first(*nats, store, publisher, DecisionEngineConfig{});
    first.run_for_context(live_ctx(pair, 1));
    const int64_t opened = rows_for_pair("strategy_candidates", pair);
    REQUIRE(opened > 0);

    // What used to happen: a fresh process knows nothing and re-issues them.
    DecisionEngine forgetful(*nats, store, publisher, DecisionEngineConfig{});
    forgetful.run_for_context(live_ctx(pair, 2));
    const int64_t reissued = rows_for_pair("strategy_candidates", pair) - opened;
    CHECK(reissued > 0);

    // A restarted process restores its book from the archive first.
    DecisionEngine restarted(*nats, store, publisher, DecisionEngineConfig{});
    restarted.restore_open_hypotheses(wall_ms());
    restarted.run_for_context(live_ctx(pair, 3));
    CHECK(rows_for_pair("strategy_candidates", pair) == opened + reissued);
}

namespace {

// A matured BALANCED proposal of `pair`: LONG by MARKET at 100, stop 99,
// target 102, 30-minute time stop, created at `t0` -- archived like a live one.
void archive_past_proposal(DecisionStore& store, int64_t pair, int64_t t0) {
    StrategyCandidate c;
    c.candidate_id = "cand_it_mat_" + std::to_string(pair);
    c.natural_key = c.candidate_id;
    c.canonical_pair_id = pair;
    c.strategy_id = "momentum_continuation";
    c.strategy_version = "1.0.0";
    c.side = Side::long_;
    c.created_at_ms = t0;
    c.context_id = "ctx-it-mat";
    TradeSignalProposal s;
    s.signal_proposal_id = "sig_it_mat_" + std::to_string(pair);
    s.natural_key = s.signal_proposal_id;
    s.candidate_id = c.candidate_id;
    s.canonical_pair_id = pair;
    s.strategy_id = c.strategy_id;
    s.strategy_version = c.strategy_version;
    s.side = Side::long_;
    s.plan_version = 1;
    s.plan_geometry_version = kPlanGeometryVersion;
    s.created_at_ms = t0;
    s.valid_until_ms = t0 + 5 * 60'000;
    s.entry_type = EntryType::market;
    s.entry_price = 100.0;
    s.entry_fill_timeout_ms = 5 * 60'000;
    s.stop_loss_price = 99.0;
    s.stop_distance_bps = 100.0;
    s.take_profit_price = 102.0;
    s.target_distance_bps = 200.0;
    s.max_holding_time_ms = 30 * 60'000;
    s.time_stop_at_ms = t0 + 30 * 60'000;
    s.cost_model_version = 2;
    s.round_trip_cost_bps = 10.0;
    REQUIRE(store.archive_decision(c, {}, {s}, {}).has_value());
}

// One candle per minute of [t0, t0 + n min): flat at `px` unless overridden,
// and -- when `trades` -- the minute's open, high, low and close as raw trades.
void write_path(int64_t pair, int64_t t0, int n, double px, const std::map<int, std::array<double, 4>>& override,
                bool trades, int skip_minute = -1) {
    std::string sql;
    for (int m = 0; m < n; ++m) {
        if (m == skip_minute) continue;
        auto ohlc = std::array<double, 4>{px, px, px, px};
        if (const auto it = override.find(m); it != override.end()) ohlc = it->second;
        const int64_t t = t0 + m * 60'000LL;
        sql += "INSERT INTO public.market_candles_1m(exchange_id,instrument_id,open_time,open,high,low,close,"
               "base_volume) VALUES (1," + std::to_string(pair) + ",to_timestamp(" + std::to_string(t) +
               "/1000.0)," + std::to_string(ohlc[0]) + "," + std::to_string(ohlc[1]) + "," +
               std::to_string(ohlc[2]) + "," + std::to_string(ohlc[3]) + ",1);\n";
        if (!trades) continue;
        int k = 0;
        for (const double p : {ohlc[0], ohlc[1], ohlc[2], ohlc[3]})
            sql += "INSERT INTO public.market_trades_raw(exchange_id,instrument_id,trade_time,price) VALUES (1," +
                   std::to_string(pair) + ",to_timestamp(" + std::to_string(t + 1'000 + 10'000 * k++) +
                   "/1000.0)," + std::to_string(p) + ");\n";
    }
    REQUIRE(env().admin_sql("altstrategy", sql));
}

} // namespace

TEST_CASE("live maturation resolves through evaluator 2, versioned, and queues its publication",
          "[integration][outcomes][v2]") {
    REQUIRE_ENV();
    // Local stand-ins with the production foreign tables' names and columns.
    REQUIRE(env().admin_sql("altstrategy",
        "CREATE TABLE IF NOT EXISTS public.market_candles_1m (exchange_id smallint, instrument_id bigint, "
        "open_time timestamptz, open numeric, high numeric, low numeric, close numeric, base_volume numeric, "
        "quote_volume numeric, is_closed boolean DEFAULT true, quality_status text DEFAULT 'ok');"
        "CREATE TABLE IF NOT EXISTS public.market_trades_raw (exchange_id smallint, instrument_id bigint, "
        "trade_time timestamptz, price numeric);"
        "GRANT SELECT ON public.market_candles_1m, public.market_trades_raw TO altstrategy_app;"));
    auto nats = connect_nats();
    persistence::PgPool pool(env().dsn(), 4);
    DecisionStore store(pool);
    MarketReader reader(pool, MarketReaderConfig{});
    SnapshotExporter exporter(pool, ExporterConfig{});
    MaintenanceScheduler scheduler(store, reader, exporter, *nats, SchedulerConfig{});
    const int64_t now = wall_ms();
    const int64_t t0 = (now - 2 * 3600'000) / 60'000 * 60'000;
    const std::map<int, std::array<double, 4>> target_at_3 = {{3, {100.4, 102.5, 100.3, 102.2}}};
    archive_past_proposal(store, 9101, t0);   // candles only
    write_path(9101, t0, 40, 100.1, target_at_3, false);
    archive_past_proposal(store, 9102, t0);   // candles and the raw tape
    write_path(9102, t0, 40, 100.1, target_at_3, true);
    archive_past_proposal(store, 9103, t0);   // the first open is past the target
    write_path(9103, t0, 40, 102.3, {}, false);
    const int64_t old = (now - 8 * 3600'000) / 60'000 * 60'000;
    archive_past_proposal(store, 9104, old);  // a missing minute after entry, past the grace
    write_path(9104, old, 40, 100.1, {{20, {100.1, 102.5, 100.0, 102.2}}}, false, 10);

    REQUIRE(scheduler.mature_outcomes(now) == 4);
    const auto row = [&](int64_t pair, const std::string& cols) {
        pqxx::connection conn(env().admin_dsn());
        pqxx::work w(conn);
        return w.exec1("SELECT " + cols + " FROM altstrategy.market_outcomes WHERE signal_proposal_id = 'sig_it_mat_" +
                       std::to_string(pair) + "'");
    };
    const auto a = row(9101, "outcome, path_source, path_bar_ms, evaluator_version, resolution, "
                             "realized_net_return IS NOT NULL, fill_model");
    CHECK(a[0].as<std::string>() == "TARGET_FIRST");
    CHECK(a[1].as<std::string>() == "PATH_1M_CANDLES");   // no raw tape: the candles, said so
    CHECK(a[2].as<int>() == 60'000);
    CHECK(a[3].as<int>() == kOutcomeEvaluatorVersion);
    CHECK(a[4].as<std::string>() == "target");
    CHECK(a[5].as<bool>());
    CHECK(a[6].as<std::string>() == kOutcomeFillModel);
    const auto b = row(9102, "outcome, path_source, path_bar_ms");
    CHECK(b[0].as<std::string>() == "TARGET_FIRST");
    CHECK(b[1].as<std::string>() == "PATH_RAW_TRADES");
    CHECK(b[2].as<int>() == 1'000);
    const auto c = row(9103, "outcome, realized_net_return IS NULL, realized_gross_return IS NULL, mfe IS NULL");
    CHECK(c[0].as<std::string>() == "ENTRY_EXPIRED");
    CHECK(c[1].as<bool>());   // no position: no return, not 0.0
    CHECK(c[2].as<bool>());
    CHECK(c[3].as<bool>());
    const auto d = row(9104, "outcome, resolution");
    CHECK(d[0].as<std::string>() == "DATA_INCOMPLETE");
    CHECK(d[1].as<std::string>() == "gap_before_resolution");

    // Each outcome owes the bus one market_outcome message.
    CHECK(env().count("SELECT count(*) FROM altstrategy.publication_outbox WHERE kind = 'market_outcome' "
                      "AND signal_proposal_id LIKE 'sig_it_mat_%'") == 4);
    // Learning sees the two resolved trades and nothing else.
    int learned = 0;
    for (const auto& s : store.load_outcome_samples(20000, 0, 2))
        if (s.canonical_pair_id >= 9101 && s.canonical_pair_id <= 9104) ++learned;
    CHECK(learned == 2);
    // Matured once: a second pass finds nothing left to do.
    CHECK(scheduler.mature_outcomes(now) == 0);
}

TEST_CASE("the service knows the schema it needs, and reads how far the migrator got", "[integration][release]") {
    REQUIRE_ENV();
    // The required version is the newest migration in the tree: a migration
    // added without moving it would let this binary start on a schema it
    // cannot write.
    std::string newest_file;
    for (const auto& e : fs::directory_iterator(fs::path(ALTSTRATEGY_SOURCE_DIR) / "db" / "migrations"))
        newest_file = std::max(newest_file, e.path().filename().string().substr(0, 4));
    CHECK(newest_file == kRequiredSchemaMigration);

    persistence::PgPool pool(env().dsn(), 2);
    DecisionStore store(pool);
    // This cluster was migrated statement by statement, so there is no ledger.
    CHECK(store.newest_schema_migration() == std::optional<std::string>(""));
    REQUIRE(env().admin_sql("altstrategy",
        "CREATE TABLE public.schema_migrations (version TEXT PRIMARY KEY, name TEXT NOT NULL, "
        "checksum TEXT NOT NULL, applied_at TIMESTAMPTZ NOT NULL DEFAULT now());"
        "GRANT SELECT ON public.schema_migrations TO altstrategy_app;"
        "INSERT INTO public.schema_migrations (version, name, checksum) VALUES "
        "('0009', 'ml_first_economic_layer', 'x'), ('0010', 'vps1_signal_semantics', 'x');"));
    const auto before = store.newest_schema_migration();
    REQUIRE(before.has_value());
    CHECK(*before == "0010");
    CHECK(*before < kRequiredSchemaMigration);   // production as it stands: refused
    REQUIRE(env().admin_sql("altstrategy",
        "INSERT INTO public.schema_migrations (version, name, checksum) VALUES ('0013', 'versioned_outcomes', 'x');"));
    CHECK(store.newest_schema_migration() == std::optional<std::string>(std::string(kRequiredSchemaMigration)));
    REQUIRE(env().admin_sql("altstrategy", "DROP TABLE public.schema_migrations;"));
}

TEST_CASE("retries back off exponentially to a cap", "[outbox]") {
    OutboxPublisherConfig cfg;
    CHECK(outbox_retry_delay_ms(1, cfg) == 1'000);
    CHECK(outbox_retry_delay_ms(2, cfg) == 2'000);
    CHECK(outbox_retry_delay_ms(3, cfg) == 4'000);
    CHECK(outbox_retry_delay_ms(6, cfg) == 32'000);
    CHECK(outbox_retry_delay_ms(7, cfg) == 60'000);
    CHECK(outbox_retry_delay_ms(1'000, cfg) == 60'000);
}
