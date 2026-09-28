// The real binary, market_history only, through every way its ownership of
// ingestion runs can end: power loss (rows in the database, no process), a
// second incarnation next to a live one, the owner's database session cut while
// its process lives (what a PostgreSQL restart does to it), the owner process
// killed while a standby waits, and a clean stop. A bar_aggregation process
// runs alongside the whole time and must stay bounded through all of it.
//
// The venue is unreachable on purpose (a .invalid host, RFC 6761): every fetch
// fails and is retried with backoff, which holds a bootstrap and a repair in
// 'running' for as long as the test needs them there, without a network and
// without touching anything that listens on this machine.

#include "messaging/codec.hpp"
#include "messaging/nats_client.hpp"
#include "messaging/subjects.hpp"
#include "components/history_runs.hpp"
#include "test_env.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

using namespace altspotter;
using namespace altspotter::test;

namespace {

constexpr int64_t kMinuteMs = 60'000;
constexpr int16_t kBitget = 1;
constexpr int64_t kInstr = 9201;
const std::string kSymbol = "ORPHUSDT";

std::string iso(int64_t ms) { return "to_timestamp(" + std::to_string(ms) + "::bigint / 1000.0)"; }

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string text(const std::string& dsn, const std::string& sql) {
    pqxx::connection c{dsn};
    pqxx::nontransaction tx{c};
    const auto r = tx.exec(sql);
    return r.empty() || r[0][0].is_null() ? "NULL" : r[0][0].as<std::string>();
}

int64_t number(const std::string& dsn, const std::string& sql) {
    const auto t = text(dsn, sql);
    return t == "NULL" ? -1 : std::stoll(t);
}

std::set<int64_t> ids(const std::string& dsn, const std::string& where) {
    pqxx::connection c{dsn};
    pqxx::nontransaction tx{c};
    std::set<int64_t> out;
    for (const auto& r : tx.exec("SELECT id FROM ingestion_runs WHERE " + where)) {
        out.insert(r[0].as<int64_t>());
    }
    return out;
}

void exec(const std::string& dsn, const std::string& sql) {
    pqxx::connection c{dsn};
    pqxx::work tx{c};
    tx.exec(sql);
    tx.commit();
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int64_t occurrences(const std::string& path, const std::string& needle) {
    const std::string log = read_file(path);
    int64_t n = 0;
    for (auto pos = log.find(needle); pos != std::string::npos; pos = log.find(needle, pos + 1)) ++n;
    return n;
}

int time_scale() {
    if (const char* v = std::getenv("ALTSPOTTER_TEST_TIME_SCALE")) {
        const int s = std::atoi(v);
        if (s > 0) return s;
    }
    return 1;
}

// Bounded wait for a condition, polled every 100 ms. `during` runs on every
// poll, so an invariant can be sampled for the whole wait.
bool eventually(int timeout_sec, const std::function<bool()>& cond,
                const std::function<void()>& during = {}) {
    for (int i = 0; i < timeout_sec * 10; ++i) {
        if (during) during();
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return cond();
}

const std::string kLockFilter =
    "locktype = 'advisory' AND granted AND objsubid = 2"
    " AND database = (SELECT oid FROM pg_database WHERE datname = current_database())"
    " AND classid = " + std::to_string(history::kRunOwnerLockSpace) +
    "::oid AND objid = " + std::to_string(history::kRunOwnerLockKey) + "::oid";

int64_t lock_holders(const std::string& dsn) {
    return number(dsn, "SELECT count(*) FROM pg_locks WHERE " + kLockFilter);
}

// Cut the owner's session server side, as a PostgreSQL restart or a network
// cut does, while its process carries on.
void cut_owner_session(const std::string& dsn) {
    const std::string pid = text(dsn, "SELECT pid FROM pg_locks WHERE " + kLockFilter);
    REQUIRE(pid != "NULL");
    REQUIRE(text(dsn, "SELECT pg_terminate_backend(" + pid + ")") == "t");
}

std::string repair_command(int64_t from_ms, int64_t to_ms) {
    messaging::EnvelopeMeta meta;
    meta.event_type = "history.repair.command";
    meta.exchange_id = kBitget;
    meta.instrument_id = kInstr;
    meta.exchange_symbol = kSymbol;
    meta.source_service = "itest";
    meta.message_id = "itest-orphan-repair";
    auto env = messaging::make_envelope(meta);
    auto* cmd = env.mutable_repair_command();
    cmd->set_gap_id(7);
    cmd->set_timeframe(altspotter::v1::TF_1M);
    cmd->set_from_ms(from_ms);
    cmd->set_to_ms(to_ms);
    cmd->set_attempt(1);
    return messaging::serialize(env);
}

struct Pass {
    std::string mode;
    int64_t deep_keys = 0, failed = 0, catchup_ms = 0, widest_span_min = 0;
};

std::vector<Pass> passes(const std::string& log) {
    std::vector<Pass> out;
    const std::regex line(R"(\{[^\n]*"event":"agg_catchup_pass"[^\n]*\})");
    const auto num = [](const std::string& s, const char* key) -> int64_t {
        std::smatch m;
        const std::regex r(std::string("\"") + key + R"(":(-?\d+))");
        return std::regex_search(s, m, r) ? std::stoll(m[1]) : -1;
    };
    for (auto it = std::sregex_iterator(log.begin(), log.end(), line); it != std::sregex_iterator();
         ++it) {
        const std::string s = it->str();
        Pass p;
        std::smatch m;
        if (std::regex_search(s, m, std::regex(R"re("mode":"(\w+)")re"))) p.mode = m[1];
        p.deep_keys = num(s, "deep_keys");
        p.failed = num(s, "failed");
        p.catchup_ms = num(s, "catchup_ms");
        p.widest_span_min = num(s, "widest_span_min");
        out.push_back(p);
    }
    return out;
}

// The ownership events of one process, in order: what actually happened.
void print_timeline(const std::string& path, const char* who) {
    static const char* kEvents[] = {
        "history_owner_acquired", "history_standby", "history_orphan_recovery",
        "history_run_orphaned", "history_run_ownership_lost", "history_run_write_fenced",
        "bootstrap_finish_not_recorded", "repair_finish_not_recorded", "history_owner_abdicated",
        "component_restart", "bootstrap_started", "repair_started", "service_stopped"};
    std::istringstream in(read_file(path));
    std::string line;
    const std::regex field(R"re("(timestamp|event|closed|run_id|kind|context|fenced)":("[^"]*"|-?\d+|true|false))re");
    while (std::getline(in, line)) {
        bool wanted = false;
        for (const char* e : kEvents) {
            if (line.find(std::string("\"event\":\"") + e + "\"") != std::string::npos) wanted = true;
        }
        if (!wanted) continue;
        std::string out;
        for (auto it = std::sregex_iterator(line.begin(), line.end(), field);
             it != std::sregex_iterator(); ++it) {
            out += (*it)[1].str() == "timestamp" ? (*it)[2].str().substr(12, 12) + " "
                                                 : (*it)[1].str() + "=" + (*it)[2].str() + " ";
        }
        std::printf("TIMELINE %s %s\n", who, out.c_str());
    }
}

// Everything the scenarios share: a database with one Bitget key that has two
// days of 1m history, JetStream, and the config both binaries run with.
struct Rig {
    TempNats nats;
    TempDb db;
    std::string bin, dsn, common_path, config_path;
    std::filesystem::path dir;
    int scale = time_scale();
    int64_t t0 = now_ms();

    bool up(const std::string& name) {
        if (!nats.start() || !db.create()) return false;
        bin = bin_dir() + "/altspotter";
        dsn = db.dsn();
        if (run_tool(bin, {"migrate", "--dsn", dsn, "--migrations", migrations_dir()}) != 0) return false;
        if (run_tool(bin, {"nats-init", "--url", nats.url()}) != 0) return false;
        exec(dsn, "INSERT INTO instruments (id, base_asset, quote_asset) VALUES (" +
                      std::to_string(kInstr) + ", 'ORPH', 'USDT')");
        exec(dsn, "INSERT INTO exchange_instruments (exchange_id, instrument_id, exchange_symbol,"
                  " status, is_whitelisted) VALUES (1, " + std::to_string(kInstr) + ", '" + kSymbol +
                  "', 'active', true)");
        const int64_t end = (t0 / kMinuteMs) * kMinuteMs;
        exec(dsn, "INSERT INTO market_candles_1m (exchange_id, instrument_id, open_time, close_time,"
                  " open, high, low, close, base_volume, quote_volume, is_closed, source)"
                  " SELECT 1, " + std::to_string(kInstr) + ", t, t + interval '1 minute',"
                  " 100,100,100,100,1,100, true, 'ws' FROM generate_series(" +
                  iso(end - 2 * 86'400'000LL) + ", " + iso(end - kMinuteMs) + ", interval '1 minute') t");
        // IDs the service assigns must not collide with rows a test inserts.
        exec(dsn, "SELECT setval(pg_get_serial_sequence('ingestion_runs', 'id'), 1000)");
        dir = std::filesystem::temp_directory_path() /
              (name + "_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        common_path = (dir / "common.yaml").string();
        config_path = (dir / "altspotter.yaml").string();
        std::ofstream common(common_path);
        common << "environment: test\nlog_level: info\ntimezone: UTC\ninstance_id: itest\n"
               << "nats_url: " << nats.url() << "\npostgres_dsn: " << dsn
               << "\nshutdown_timeout_sec: 10\n";
        std::ofstream cfg(config_path);
        cfg << "exchanges:\n  bitget: { enabled: true }\n  binance_usdm: { enabled: false }\n"
            << "  bybit: { enabled: false }\n  okx: { enabled: false }\n"
            << "market_history:\n  bootstrap_days: 1\n  reconcile_interval_sec: 3600\n"
            << "  postgres_pool_size: 3\n  bitget:\n"
            << "    rest_base_url: https://altspotter-test.invalid\n"
            << "    product_type: USDT-FUTURES\n    connect_timeout_ms: 1000\n"
            << "    request_timeout_ms: 1000\n    rate_limit_per_second: 5\n"
            << "    max_retries: 10\n    backoff_min_ms: 5000\n    backoff_max_ms: 10000\n"
            << "storage_writer:\n  consumer:\n    stream: ALTSPOTTER_BARS\n"
            << "    durable: storage-writer-bars\n    fetch_batch: 256\n    fetch_timeout_ms: 300\n"
            << "  batch_size: 2000\n  postgres_pool_size: 2\n"
            << "bar_aggregation:\n  timeframes: [5m, 15m, 30m, 1h, 4h, 1d]\n"
            << "  cycle_interval_sec: 2\n  close_delay_sec: 5\n  recheck_horizon_min: 35\n"
            << "  catchup_interval_sec: 10\n  publish_batch_size: 500\n  postgres_pool_size: 2\n";
        return true;
    }
    ~Rig() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::unique_ptr<ChildProcess> start(const std::string& components, const std::string& log) {
        return std::make_unique<ChildProcess>(
            bin,
            std::vector<std::string>{"run", "--only", components, "--config", config_path,
                                     "--common", common_path},
            (dir / log).string());
    }
    std::string log(const std::string& name) const { return (dir / name).string(); }

    int64_t running(const std::string& kind) const {
        return number(dsn, "SELECT count(*) FROM ingestion_runs WHERE kind = '" + kind +
                               "' AND status = 'running'");
    }
};

// What a bar_aggregation process that ran through a scenario must show: every
// periodic pass bounded -- nothing wider than the one bootstrap
// (bootstrap_days = 1) plus the recent range, no pass holding the loop -- and
// no writer trouble. A key reaches back to the floor only until its first
// successful pass: a key whose pass failed keeps the frontier it had, and one
// that never had one (its startup pass failed, e.g. because the database went
// down under it) still owes its startup recovery. So deep keys in a pass are
// bounded by the keys the previous pass failed.
void check_bounded(const std::string& log_path, int64_t started_ms, int scale,
                   bool database_went_down = false) {
    const std::string log = read_file(log_path);
    const auto all = passes(log);
    REQUIRE(all.size() >= 3);
    const int64_t elapsed_min = (now_ms() - started_ms) / kMinuteMs + 1;
    int64_t worst = 0, widest = 0, deep_after_failure = 0;
    for (size_t i = 1; i < all.size(); ++i) {
        CAPTURE(i, all[i].mode, all[i].deep_keys, all[i].failed, all[i - 1].failed,
                all[i].catchup_ms, all[i].widest_span_min);
        CHECK(all[i].mode == "periodic");
        CHECK(all[i].deep_keys <= all[i - 1].failed);
        // A pass that ran while the database was down fails its keys and keeps
        // their frontiers for the next pass; that is the only excuse.
        if (!database_went_down) CHECK(all[i].failed == 0);
        if (all[i].deep_keys == 0) {
            CHECK(all[i].widest_span_min <= 1440 + 36 + elapsed_min);
            widest = std::max(widest, all[i].widest_span_min);
        } else {
            deep_after_failure += all[i].deep_keys;
        }
        worst = std::max(worst, all[i].catchup_ms);
    }
    CHECK(all.back().failed == 0);
    CHECK(all.back().deep_keys == 0);
    std::printf("BOUNDED passes=%zu worst_periodic_catchup_ms=%lld widest_bounded_span_min=%lld "
                "deep_after_failed_pass=%lld\n",
                all.size(), static_cast<long long>(worst), static_cast<long long>(widest),
                static_cast<long long>(deep_after_failure));
    CHECK(worst < 2'000 * scale);   // the loop is never held: live cycles keep running
    CHECK(log.find("writer_batch_failed_isolating") == std::string::npos);
    CHECK(log.find("no partition of relation") == std::string::npos);
}

} // namespace

TEST_CASE("run ownership survives power loss, a second process, a cut session and a killed owner",
          "[integration][history][orphans]") {
    if (!TempNats::server_binary()) {
        SKIP("nats-server binary not available (set ALTSPOTTER_TEST_NATS_SERVER)");
    }
    if (!TempDb::admin_dsn()) {
        SKIP("PostgreSQL not available (set ALTSPOTTER_TEST_PG_DSN)");
    }
    Rig rig;
    REQUIRE(rig.up("altspotter_owner"));
    const std::string& dsn = rig.dsn;
    const int scale = rig.scale;
    const int64_t t0 = rig.t0;

    // ---- the power-loss model ----------------------------------------------
    // Rows a previous boot committed; no process, no session. The coverage was
    // touched a minute ago, so the 30-minute staleness rule alone would leave
    // the bootstrap parked for half an hour.
    exec(dsn, "INSERT INTO ingestion_runs (id, service, kind, exchange_id, instrument_id,"
              " requested_from, requested_to, status, started_at) VALUES"
              " (101, 'market-history-service', 'repair', 1, " + std::to_string(kInstr) + ", " +
              iso(t0 - 300 * kMinuteMs) + ", " + iso(t0 - 241 * kMinuteMs) + ", 'running', " +
              iso(t0 - 2 * kMinuteMs) + "),"
              " (102, 'market-history-service', 'bootstrap', 1, " + std::to_string(kInstr) + ", " +
              iso(t0 - 86'400'000) + ", " + iso(t0 - kMinuteMs) + ", 'running', " +
              iso(t0 - kMinuteMs) + ")");
    exec(dsn, "INSERT INTO history_coverage (exchange_id, instrument_id, timeframe, status,"
              " requested_from, updated_at) VALUES (1, " + std::to_string(kInstr) +
              ", '1m', 'bootstrapping', " + iso(t0 - 86'400'000) + ", now() - interval '1 minute')");
    REQUIRE(lock_holders(dsn) == 0);

    // The aggregation observer runs through every phase.
    auto q = rig.start("bar_aggregation,storage_writer", "q.log");
    const auto p1_log = rig.log("p1.log");
    const auto p2_log = rig.log("p2.log");
    // Never two bootstraps, never two repairs 'running' at once: sampled on
    // every poll of every wait below.
    int64_t max_bootstraps = 0, max_repairs = 0, max_holders = 0;
    const auto sample = [&] {
        max_bootstraps = std::max(max_bootstraps, rig.running("bootstrap"));
        max_repairs = std::max(max_repairs, rig.running("repair"));
        max_holders = std::max(max_holders, lock_holders(dsn));
    };

    // ---- phase 1 / test 15: power loss -----------------------------------------
    auto p1 = rig.start("market_history", "p1.log");
    REQUIRE(eventually(30 * scale, [&] { return occurrences(p1_log, "\"event\":\"history_orphan_recovery\"") == 1; }, sample));
    CHECK(occurrences(p1_log, "\"closed\":2") == 1);
    CHECK(number(dsn, "SELECT count(*) FROM ingestion_runs WHERE id IN (101, 102)"
                      " AND status = 'aborted' AND error LIKE 'orphaned:%'") == 2);
    REQUIRE(eventually(30 * scale, [&] { return rig.running("bootstrap") == 1; }, sample));

    const int64_t rep_from = (t0 / kMinuteMs) * kMinuteMs - 180 * kMinuteMs;
    const int64_t rep_to = rep_from + 59 * kMinuteMs;
    {
        auto client = messaging::NatsClient::connect(rig.nats.url(), "itest-orphans");
        REQUIRE(client.has_value());
        REQUIRE((*client)->js_publish(subjects::repair_command("bitget", kSymbol),
                                      repair_command(rep_from, rep_to), "itest-orphan-repair")
                    .has_value());
    }
    REQUIRE(eventually(30 * scale, [&] { return rig.running("repair") == 1; }, sample));
    const auto runs_of = [&](const std::string& kind, const std::string& range) {
        return ids(dsn, "kind = '" + kind + "'" + range);
    };
    const std::string this_repair = " AND requested_from = " + iso(rep_from) +
                                    " AND requested_to = " + iso(rep_to);

    // ---- phase 2 / test 16: a second process next to a live owner ------------
    const auto p1_runs = ids(dsn, "status = 'running'");
    REQUIRE(p1_runs.size() == 2);
    const int64_t rows_before_p2 = number(dsn, "SELECT count(*) FROM ingestion_runs");
    auto p2 = rig.start("market_history", "p2.log");
    REQUIRE(eventually(30 * scale, [&] { return occurrences(p2_log, "\"event\":\"history_standby\"") == 1; }, sample));
    // A few of its standby ticks: it takes nothing, starts nothing, closes nothing.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    sample();
    CHECK(occurrences(p2_log, "\"event\":\"history_owner_acquired\"") == 0);
    CHECK(ids(dsn, "status = 'running'") == p1_runs);
    CHECK(number(dsn, "SELECT count(*) FROM ingestion_runs") == rows_before_p2);
    CHECK(lock_holders(dsn) == 1);

    // ---- phase 3 / test 1: the owner's session is cut, both processes alive ---
    // P2 (standby) takes over within a tick; P1 is fenced, notices, stops and
    // restarts into standby. P1's runs are closed by P2 and stay closed.
    cut_owner_session(dsn);
    REQUIRE(eventually(15 * scale, [&] { return occurrences(p2_log, "\"event\":\"history_orphan_recovery\"") == 1; }, sample));
    CHECK(occurrences(p2_log, "\"closed\":2") == 1);
    REQUIRE(eventually(30 * scale, [&] { return occurrences(p1_log, "\"event\":\"history_owner_abdicated\"") == 1; }, sample));
    CHECK(occurrences(p1_log, "\"event\":\"history_run_ownership_lost\"") == 1);
    REQUIRE(eventually(30 * scale, [&] { return occurrences(p1_log, "\"event\":\"history_standby\"") == 1; }, sample));
    for (const int64_t id : p1_runs) {
        CHECK(text(dsn, "SELECT status FROM ingestion_runs WHERE id = " + std::to_string(id)) == "aborted");
    }
    // P2 carries the work on: the bootstrap is resumed, and the repair P1 was
    // fenced from recording is redelivered (P1 NAKed it) and run by P2.
    const int64_t after_cut = *std::max_element(p1_runs.begin(), p1_runs.end());
    REQUIRE(eventually(30 * scale, [&] { return !runs_of("bootstrap", " AND status = 'running' AND id > " + std::to_string(after_cut)).empty(); }, sample));
    REQUIRE(eventually(60 * scale, [&] { return !runs_of("repair", this_repair + " AND id > " + std::to_string(after_cut)).empty(); }, sample));
    for (const int64_t id : p1_runs) {   // still P2's verdict, after P1 stopped
        CHECK(text(dsn, "SELECT status FROM ingestion_runs WHERE id = " + std::to_string(id)) == "aborted");
    }
    CHECK(lock_holders(dsn) == 1);
    CHECK(occurrences(p1_log, "\"event\":\"history_owner_acquired\"") == 1);   // only its first

    // ---- phase 4 / test 14: the owner process dies, a standby takes over ------
    REQUIRE(eventually(30 * scale, [&] { return rig.running("repair") == 1; }, sample));
    const auto p2_runs = ids(dsn, "status = 'running'");
    p2->terminate(SIGKILL);
    REQUIRE(eventually(15 * scale, [&] { return occurrences(p1_log, "\"event\":\"history_orphan_recovery\"") == 2; }, sample));
    for (const int64_t id : p2_runs) {
        CHECK(text(dsn, "SELECT status FROM ingestion_runs WHERE id = " + std::to_string(id)) == "aborted");
    }
    // test 17: the repair P2 died holding is redelivered after AckWait and P1
    // runs it -- not lost.
    const int64_t after_kill = *std::max_element(p2_runs.begin(), p2_runs.end());
    REQUIRE(eventually(120 * scale, [&] { return !runs_of("repair", this_repair + " AND id > " + std::to_string(after_kill)).empty(); }, sample));
    CHECK(lock_holders(dsn) == 1);

    // ---- phase 5: the only owner's session is cut; it comes back by itself ----
    // Whatever P1 is running now (the redelivered repair at least; its
    // bootstrap may already have given up on the unreachable venue).
    const auto p1b_runs = ids(dsn, "status = 'running'");
    REQUIRE_FALSE(p1b_runs.empty());
    cut_owner_session(dsn);
    REQUIRE(eventually(30 * scale, [&] { return occurrences(p1_log, "\"event\":\"history_owner_abdicated\"") == 2; }, sample));
    REQUIRE(eventually(60 * scale, [&] { return occurrences(p1_log, "\"event\":\"history_orphan_recovery\"") == 3; }, sample));
    for (const int64_t id : p1b_runs) {   // its own runs: stopped before it took the lock again
        CHECK(text(dsn, "SELECT status FROM ingestion_runs WHERE id = " + std::to_string(id)) == "aborted");
    }
    REQUIRE(eventually(30 * scale, [&] { return rig.running("bootstrap") == 1; }, sample));

    // ---- phase 6: a clean stop still closes its own runs itself ---------------
    p1->terminate(SIGTERM);
    sample();
    CHECK(rig.running("bootstrap") + rig.running("repair") == 0);
    CHECK(eventually(10 * scale, [&] { return lock_holders(dsn) == 0; }));

    CAPTURE(max_bootstraps, max_repairs, max_holders);
    CHECK(max_bootstraps <= 1);   // no duplicate bootstrap, ever
    CHECK(max_repairs <= 1);      // no duplicate repair, ever
    CHECK(max_holders <= 1);      // never two owners

    q->terminate(SIGTERM);
    print_timeline(p1_log, "P1");
    print_timeline(p2_log, "P2");
    check_bounded(rig.log("q.log"), t0, scale);
}

// A real PostgreSQL restart under a live process. Needs a command that
// restarts the cluster behind ALTSPOTTER_TEST_PG_DSN
// (ALTSPOTTER_TEST_PG_RESTART_CMD, e.g. `pg_ctl -D <data> -m fast -w restart`),
// so it is skipped where there is none.
TEST_CASE("a PostgreSQL restart under a live owner fences it; it comes back as the only owner",
          "[integration][history][orphans][pg-restart]") {
    const char* restart = std::getenv("ALTSPOTTER_TEST_PG_RESTART_CMD");
    if (restart == nullptr || !TempNats::server_binary() || !TempDb::admin_dsn()) {
        SKIP("set ALTSPOTTER_TEST_PG_RESTART_CMD, ALTSPOTTER_TEST_NATS_SERVER and ALTSPOTTER_TEST_PG_DSN");
    }
    Rig rig;
    REQUIRE(rig.up("altspotter_pgrestart"));
    const std::string& dsn = rig.dsn;
    const int scale = rig.scale;
    auto q = rig.start("bar_aggregation,storage_writer", "q.log");
    auto p1 = rig.start("market_history", "p1.log");
    const auto p1_log = rig.log("p1.log");
    int64_t max_bootstraps = 0, max_holders = 0;
    const auto sample = [&] {
        try {   // the database is down for part of this test
            max_bootstraps = std::max(max_bootstraps, rig.running("bootstrap"));
            max_holders = std::max(max_holders, lock_holders(dsn));
        } catch (const std::exception&) {
        }
    };
    REQUIRE(eventually(30 * scale, [&] { return occurrences(p1_log, "\"event\":\"history_owner_acquired\"") == 1; }, sample));
    REQUIRE(eventually(30 * scale, [&] { return rig.running("bootstrap") == 1; }, sample));
    const auto before = ids(dsn, "status = 'running'");
    const size_t passes_before = passes(read_file(rig.log("q.log"))).size();

    REQUIRE(std::system(restart) == 0);

    REQUIRE(eventually(60 * scale, [&] { return occurrences(p1_log, "\"event\":\"history_owner_abdicated\"") == 1; }, sample));
    REQUIRE(eventually(120 * scale, [&] { return occurrences(p1_log, "\"event\":\"history_owner_acquired\"") == 2; }, sample));
    for (const int64_t id : before) {
        CHECK(text(dsn, "SELECT status FROM ingestion_runs WHERE id = " + std::to_string(id)) == "aborted");
    }
    REQUIRE(eventually(30 * scale, [&] { return rig.running("bootstrap") == 1; }, sample));
    CHECK(lock_holders(dsn) == 1);
    p1->terminate(SIGTERM);
    CHECK(rig.running("bootstrap") + rig.running("repair") == 0);
    CAPTURE(max_bootstraps, max_holders);
    CHECK(max_bootstraps <= 1);
    CHECK(max_holders <= 1);
    // The aggregation process went through the same restart: two more passes
    // after it, the last of them clean.
    REQUIRE(eventually(60 * scale, [&] {
        return passes(read_file(rig.log("q.log"))).size() >= std::max<size_t>(passes_before + 2, 3);
    }));
    q->terminate(SIGTERM);
    print_timeline(p1_log, "P1");
    {
        std::istringstream in(read_file(rig.log("q.log")));
        std::string line;
        while (std::getline(in, line)) {
            for (const char* e : {"agg_catchup_pass", "component_restart", "component_crashed",
                                  "postgres_unavailable", "agg_active_load_failed"}) {
                if (line.find(std::string("\"event\":\"") + e + "\"") != std::string::npos) {
                    std::printf("QLOG %s\n", line.substr(0, 400).c_str());
                }
            }
        }
    }
    check_bounded(rig.log("q.log"), rig.t0, scale, /*database_went_down=*/true);
}
