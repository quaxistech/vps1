// The real binary, bar_aggregation + storage_writer, against the shape of the
// 2026-09 incident: 1m history reaching into months whose aggregate partitions
// retention already dropped, and a catch-up schedule shortened so several
// passes run inside one test.
//
// It checks, in a running process, what the unit tests check statement by
// statement: the startup pass still recovers history from the partition floor;
// periodic passes scan only what the writer can have changed -- the recent
// range and the writable part of each recorded REST backfill -- and stay short;
// a repair below the floor, or one left 'running' by a dead process, widens
// nothing; an empty aggregate key is built from the floor, not from 'epoch'; a
// repair below the floor writes nothing; and none of it puts the writer into
// per-message isolation.

#include "messaging/codec.hpp"
#include "messaging/nats_client.hpp"
#include "messaging/subjects.hpp"
#include "test_env.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace altspotter;
using namespace altspotter::test;

namespace {

constexpr int64_t kMinuteMs = 60'000;
constexpr int64_t kDayMs = 86'400'000;

std::string iso(int64_t ms) { return "to_timestamp(" + std::to_string(ms) + "::bigint / 1000.0)"; }

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t scalar(const std::string& dsn, const std::string& sql) {
    pqxx::connection c{dsn};
    pqxx::work tx{c};
    tx.exec("SET LOCAL TIME ZONE 'UTC'");
    const auto r = tx.exec(sql);
    return r.empty() || r[0][0].is_null() ? -1 : r[0][0].as<int64_t>();
}

void exec(const std::string& dsn, const std::string& sql) {
    pqxx::connection c{dsn};
    pqxx::work tx{c};
    tx.exec("SET LOCAL TIME ZONE 'UTC'");
    tx.exec(sql);
    tx.commit();
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Pass {
    std::string mode;
    int64_t keys = 0, deep_keys = 0, windows = 0, failed = 0, duration_ms = 0, catchup_ms = 0,
            live_cycles = 0, backfill_ranges = 0, runs_below_floor = 0, runs_clamped = 0,
            widest_span_min = 0, slowest_key_ms = 0;
    bool run_owner_alive = true;
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
        p.keys = num(s, "keys");
        p.deep_keys = num(s, "deep_keys");
        p.windows = num(s, "windows");
        p.failed = num(s, "failed");
        p.duration_ms = num(s, "duration_ms");
        p.catchup_ms = num(s, "catchup_ms");
        p.live_cycles = num(s, "live_cycles");
        p.backfill_ranges = num(s, "backfill_ranges");
        p.runs_below_floor = num(s, "runs_below_floor");
        p.runs_clamped = num(s, "runs_clamped");
        p.widest_span_min = num(s, "widest_span_min");
        p.slowest_key_ms = num(s, "slowest_key_ms");
        p.run_owner_alive = s.find("\"run_owner_alive\":false") == std::string::npos;
        out.push_back(p);
    }
    return out;
}

int64_t occurrences(const std::string& log, const std::string& needle) {
    int64_t n = 0;
    for (auto pos = log.find(needle); pos != std::string::npos; pos = log.find(needle, pos + 1)) ++n;
    return n;
}

// Timeouts and absolute timing bounds scale with ALTSPOTTER_TEST_TIME_SCALE
// (default 1). A Debug build under ASan/UBSan runs this workload many times
// slower; the scale keeps its waits bounded without weakening what is checked.
// The periodic-versus-startup ratio does not scale: it holds at any speed.
int time_scale() {
    if (const char* v = std::getenv("ALTSPOTTER_TEST_TIME_SCALE")) {
        const int s = std::atoi(v);
        if (s > 0) return s;
    }
    return 1;
}

// Wait (bounded) until the log holds at least `count` passes.
bool wait_for_passes(const std::string& path, size_t count, int timeout_sec) {
    for (int i = 0; i < timeout_sec * 4; ++i) {
        if (passes(read_file(path)).size() >= count) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

std::string repair_result(int64_t instrument_id, const std::string& symbol, int64_t from_ms,
                          int64_t to_ms, const std::string& id) {
    messaging::EnvelopeMeta meta;
    meta.event_type = "history.repair.completed";
    meta.exchange_id = 1;
    meta.instrument_id = instrument_id;
    meta.exchange_symbol = symbol;
    meta.source_service = "itest";
    meta.message_id = id;
    auto env = messaging::make_envelope(meta);
    auto* res = env.mutable_repair_result();
    res->set_status(altspotter::v1::REPAIR_STATUS_COMPLETED);
    res->set_timeframe(altspotter::v1::TF_1M);
    res->set_from_ms(from_ms);
    res->set_to_ms(to_ms);
    res->set_rows_received(60);
    res->set_rows_published(60);
    return messaging::serialize(env);
}

} // namespace

TEST_CASE("catch-up on the incident shape: bounded, recovering, never poisoning",
          "[integration][aggregation][catchup]") {
    if (!TempNats::server_binary()) {
        SKIP("nats-server binary not available (set ALTSPOTTER_TEST_NATS_SERVER)");
    }
    if (!TempDb::admin_dsn()) {
        SKIP("PostgreSQL not available (set ALTSPOTTER_TEST_PG_DSN)");
    }
    TempNats nats;
    REQUIRE(nats.start());
    TempDb db;
    REQUIRE(db.create());
    const std::string bin = bin_dir() + "/altspotter";
    REQUIRE(run_tool(bin, {"migrate", "--dsn", db.dsn(), "--migrations", migrations_dir()}) == 0);
    REQUIRE(run_tool(bin, {"nats-init", "--url", nats.url()}) == 0);
    const std::string dsn = db.dsn();

    // ---- the incident shape ----------------------------------------------
    // migrate pre-creates partitions around today. Drop the two oldest
    // aggregate months, as retention under disk pressure did; their 1m
    // partitions stay.
    {
        pqxx::connection c{dsn};
        pqxx::work tx{c};
        const auto oldest = tx.exec(
            "SELECT partition_name FROM altspotter_partition_bounds('market_candles_agg'::regclass)"
            " ORDER BY lower_bound LIMIT 2");
        REQUIRE(oldest.size() == 2);
        for (const auto& r : oldest) tx.exec("DROP TABLE " + tx.quote_name(r[0].as<std::string>()));
        tx.commit();
    }
    const int64_t floor = scalar(dsn,
        "SELECT (extract(epoch FROM min(lower_bound)) * 1000)::bigint"
        " FROM altspotter_partition_bounds('market_candles_agg'::regclass)");
    REQUIRE(floor > 0);
    const int64_t band_from = floor - 10 * kDayMs;              // 10 poison days below the floor
    const int64_t history_end = (now_ms() / kMinuteMs) * kMinuteMs;  // last closed minute + 1
    // Key B: at startup the writer is an hour behind (its 1m rows stop here);
    // after the startup pass it "catches up" that hour. The older part of it is
    // outside the live recheck horizon, so only a periodic pass can aggregate it
    // -- and only if its reach follows the writer frontier.
    const int64_t writer_behind_from = history_end - 60 * kMinuteMs;

    // Three instruments on Bitget. A and B have their aggregates; E has none.
    struct Key { int64_t id; std::string symbol; bool aggregated; };
    const std::vector<Key> keys = {{9101, "AAAUSDT", true}, {9102, "BBBUSDT", true},
                                   {9103, "EEEUSDT", false}};
    for (const auto& k : keys) {
        exec(dsn, "INSERT INTO instruments (id, base_asset, quote_asset) VALUES (" +
                      std::to_string(k.id) + ", '" + k.symbol.substr(0, 3) + "', 'USDT')");
        exec(dsn, "INSERT INTO exchange_instruments (exchange_id, instrument_id, exchange_symbol,"
                  " status, is_whitelisted) VALUES (1, " + std::to_string(k.id) + ", '" + k.symbol +
                  "', 'active', true)");
        const int64_t key_end = k.id == 9102 ? writer_behind_from : history_end;
        exec(dsn, "INSERT INTO market_candles_1m (exchange_id, instrument_id, open_time, close_time,"
                  " open, high, low, close, base_volume, quote_volume, is_closed, source)"
                  " SELECT 1, " + std::to_string(k.id) + ", t, t + interval '1 minute',"
                  " 100,100,100,100,1,100, true, 'rest_bootstrap'"
                  " FROM generate_series(" + iso(band_from) + ", " + iso(key_end - kMinuteMs) +
                  ", interval '1 minute') t");
        if (!k.aggregated) continue;
        for (const auto& [tf, minutes] : std::vector<std::pair<std::string, int>>{
                 {"5m", 5}, {"15m", 15}, {"30m", 30}, {"1h", 60}, {"4h", 240}, {"1d", 1440}}) {
            exec(dsn, "INSERT INTO market_candles_agg (exchange_id, instrument_id, timeframe,"
                      " open_time, close_time, open, high, low, close, base_volume, quote_volume,"
                      " is_closed, quality_status, minute_count)"
                      " SELECT 1, " + std::to_string(k.id) + ", '" + tf + "', w, w + interval '" +
                      std::to_string(minutes) + " minutes' - interval '1 millisecond',"
                      " 100,100,100,100, count(*), 100*count(*), true, 'ok', count(*)"
                      " FROM (SELECT date_bin('" + std::to_string(minutes) +
                      " minutes', open_time, 'epoch'::timestamptz) w FROM market_candles_1m"
                      " WHERE exchange_id = 1 AND instrument_id = " + std::to_string(k.id) +
                      " AND open_time >= " + iso(floor) + ") s GROUP BY w HAVING count(*) = " +
                      std::to_string(minutes));
        }
    }
    const int64_t old_gap_1 = floor + 5 * kDayMs;   // missing before start: startup must restore it
    const int64_t old_gap_2 = floor + 7 * kDayMs;   // removed after start: periodic must leave it
    exec(dsn, "DELETE FROM market_candles_agg WHERE exchange_id = 1 AND instrument_id = 9101"
              " AND timeframe = '5m' AND open_time = " + iso(old_gap_1));
    // Production tables carry planner statistics; ~400k rows bulk-loaded a
    // second ago do not, until autovacuum gets to them. Planned without them,
    // compute_windows' join against market_candles_agg ran for minutes instead
    // of seconds, and whether it did depended on autovacuum's timing. Analyze,
    // as production effectively is, so the test measures the code and not the
    // planner's guess.
    exec(dsn, "ANALYZE market_candles_1m");
    exec(dsn, "ANALYZE market_candles_agg");

    // ---- the real binary ---------------------------------------------------
    const auto dir = std::filesystem::temp_directory_path() /
                     ("altspotter_catchup_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto common_path = dir / "common.yaml";
    const auto config_path = dir / "altspotter.yaml";
    const auto log_path = (dir / "altspotter.log").string();
    {
        std::ofstream common(common_path);
        common << "environment: test\nlog_level: info\ntimezone: UTC\ninstance_id: itest\n"
               << "nats_url: " << nats.url() << "\npostgres_dsn: " << dsn
               << "\nshutdown_timeout_sec: 10\n";
        std::ofstream cfg(config_path);
        cfg << "storage_writer:\n  consumer:\n    stream: ALTSPOTTER_BARS\n"
            << "    durable: storage-writer-bars\n    fetch_batch: 256\n    fetch_timeout_ms: 300\n"
            << "  batch_size: 2000\n  postgres_pool_size: 2\n"
            << "bar_aggregation:\n  timeframes: [5m, 15m, 30m, 1h, 4h, 1d]\n"
            << "  cycle_interval_sec: 2\n  close_delay_sec: 5\n  recheck_horizon_min: 35\n"
            << "  catchup_interval_sec: 15\n  publish_batch_size: 500\n  postgres_pool_size: 2\n";
    }
    ChildProcess proc(bin, {"run", "--only", "bar_aggregation,storage_writer", "--config",
                            config_path.string(), "--common", common_path.string()},
                      log_path);
    REQUIRE(proc.running());

    // Startup pass, then the gaps a periodic pass must and must not see.
    REQUIRE(wait_for_passes(log_path, 1, 300 * time_scale()));
    exec(dsn, "DELETE FROM market_candles_agg WHERE exchange_id = 1 AND instrument_id = 9101"
              " AND timeframe = '5m' AND open_time = " + iso(old_gap_2));
    // The writer commits key B's missing hour.
    exec(dsn, "INSERT INTO market_candles_1m (exchange_id, instrument_id, open_time, close_time,"
              " open, high, low, close, base_volume, quote_volume, is_closed, source)"
              " SELECT 1, 9102, t, t + interval '1 minute', 100,100,100,100,1,100, true, 'ws'"
              " FROM generate_series(" + iso(writer_behind_from) + ", " + iso(history_end - kMinuteMs) +
              ", interval '1 minute') t");
    // 5m windows of that hour that no live cycle can still reach (older than
    // its 35-minute recheck by the time a periodic pass runs).
    const int64_t lag_first = ((writer_behind_from + 5 * kMinuteMs - 1) / (5 * kMinuteMs)) * 5 * kMinuteMs;
    const int64_t lag_last = ((history_end - 40 * kMinuteMs) / (5 * kMinuteMs)) * 5 * kMinuteMs;
    const int64_t lag_windows = (lag_last - lag_first) / (5 * kMinuteMs);
    REQUIRE(lag_windows >= 3);

    // REST repairs as market_history records them, all finished after the
    // startup pass saw key A's frontier. Below the floor: nothing to rescan.
    // Straddling it: rescanned from the floor. A week above it: rescanned over
    // its own 1000 minutes only -- the gap inside is restored, and old_gap_2,
    // between it and now, is not. And one a killed process left 'running': no
    // market_history is alive to be running it, so it widens nothing.
    const int64_t repair_gap = floor + 3 * kDayMs + 200 * kMinuteMs;
    const int64_t orphan_gap = floor + 9 * kDayMs + 100 * kMinuteMs;
    for (const int64_t gap : {repair_gap, orphan_gap}) {
        exec(dsn, "DELETE FROM market_candles_agg WHERE exchange_id = 1 AND instrument_id = 9101"
                  " AND timeframe = '5m' AND open_time = " + iso(gap));
    }
    const auto record_repair = [&](int64_t from, int64_t minutes, const char* status) {
        const bool running = std::string(status) == "running";
        exec(dsn, "INSERT INTO ingestion_runs (service, kind, exchange_id, instrument_id,"
                  " requested_from, requested_to, status, started_at, finished_at) VALUES"
                  " ('market-history-service', 'repair', 1, 9101, " + iso(from) + ", " +
                  iso(from + (minutes - 1) * kMinuteMs) + ", '" + status + "', now(), " +
                  (running ? std::string("NULL") : std::string("now()")) + ")");
    };
    record_repair(band_from + 2 * kDayMs, 1000, "completed");
    record_repair(floor - 30 * kMinuteMs, 60, "completed");
    record_repair(floor + 3 * kDayMs, 1000, "completed");
    record_repair(floor + 9 * kDayMs, 1000, "running");

    // Repair results: one entirely below the floor, one straddling it.
    auto client = messaging::NatsClient::connect(nats.url(), "itest-repair");
    REQUIRE(client.has_value());
    REQUIRE((*client)->js_publish(subjects::repair_result("bitget", "AAAUSDT", true),
                                  repair_result(9101, "AAAUSDT", band_from + kDayMs,
                                                band_from + kDayMs + 3'600'000, "itest-rep-below"),
                                  "itest-rep-below").has_value());
    REQUIRE((*client)->js_publish(subjects::repair_result("bitget", "AAAUSDT", true),
                                  repair_result(9101, "AAAUSDT", floor - 30 * kMinuteMs,
                                                floor + 30 * kMinuteMs, "itest-rep-straddle"),
                                  "itest-rep-straddle").has_value());

    REQUIRE(wait_for_passes(log_path, 4, 120 * time_scale()));   // startup + three periodic
    std::this_thread::sleep_for(std::chrono::seconds(4));   // let the writer drain
    proc.terminate(SIGTERM);
    const std::string log = read_file(log_path);
    const auto all = passes(log);
    REQUIRE(all.size() >= 4);

    for (const auto& p : all) {
        std::printf("PASS mode=%s keys=%lld deep=%lld windows=%lld failed=%lld duration_ms=%lld "
                    "catchup_ms=%lld live_cycles=%lld backfill_ranges=%lld below_floor=%lld "
                    "clamped=%lld widest_span_min=%lld slowest_key_ms=%lld owner_alive=%d\n",
                    p.mode.c_str(), static_cast<long long>(p.keys), static_cast<long long>(p.deep_keys),
                    static_cast<long long>(p.windows), static_cast<long long>(p.failed),
                    static_cast<long long>(p.duration_ms), static_cast<long long>(p.catchup_ms),
                    static_cast<long long>(p.live_cycles), static_cast<long long>(p.backfill_ranges),
                    static_cast<long long>(p.runs_below_floor), static_cast<long long>(p.runs_clamped),
                    static_cast<long long>(p.widest_span_min), static_cast<long long>(p.slowest_key_ms),
                    p.run_owner_alive ? 1 : 0);
    }

    // ---- test 4: startup recovery -------------------------------------------
    const Pass& startup = all.front();
    CAPTURE(startup.duration_ms, startup.windows, startup.keys, startup.deep_keys);
    CHECK(startup.mode == "startup");
    CHECK(startup.keys == 3);
    CHECK(startup.deep_keys == 3);     // every key reached back to the floor
    CHECK(startup.failed == 0);
    CHECK(scalar(dsn, "SELECT count(*) FROM market_candles_agg WHERE instrument_id = 9101"
                      " AND timeframe = '5m' AND quality_status = 'ok' AND open_time = " +
                          iso(old_gap_1)) == 1);

    // ---- tests 1 and 10: periodic passes are bounded and short ---------------
    int64_t worst_periodic = 0;
    for (size_t i = 1; i < all.size(); ++i) {
        CAPTURE(i, all[i].duration_ms, all[i].catchup_ms, all[i].live_cycles, all[i].deep_keys,
                all[i].windows);
        CHECK(all[i].mode == "periodic");
        CHECK(all[i].keys == 3);
        CHECK(all[i].deep_keys == 0);  // no key went back to the floor
        CHECK(all[i].failed == 0);
        worst_periodic = std::max(worst_periodic, all[i].catchup_ms);
    }
    CAPTURE(startup.catchup_ms, worst_periodic);
    CHECK(worst_periodic < 2'000 * time_scale());
    CHECK(worst_periodic * 5 < startup.catchup_ms);

    // ---- repairs: only their writable ranges, never the history ---------------
    // Every periodic pass after the rows landed saw the three finished repairs
    // (key A's frontier does not move in this test, so they stay in reach): one
    // below the floor, skipped; one cut at the floor; one rescanned over its own
    // range. The 'running' one has no live owner and is not counted at all.
    const int64_t full_span_min = (history_end - floor) / kMinuteMs;
    // Minutes since the history ends: the recent range grows by this much.
    const int64_t elapsed_min = (now_ms() - history_end) / kMinuteMs + 1;
    for (size_t i = 1; i < all.size(); ++i) {
        CAPTURE(i, all[i].backfill_ranges, all[i].runs_below_floor, all[i].runs_clamped,
                all[i].widest_span_min, full_span_min, all[i].slowest_key_ms);
        CHECK_FALSE(all[i].run_owner_alive);
        CHECK(all[i].backfill_ranges == 2);
        CHECK(all[i].runs_below_floor == 1);
        CHECK(all[i].runs_clamped == 1);
        // Key A: the recent range (horizon plus elapsed), 1000 repaired
        // minutes and 30 minutes above the floor; key B, its hour of writer
        // lag plus the horizon, is narrower. Nothing near the ~month of
        // history a floor-bound scan would read.
        CAPTURE(elapsed_min);
        CHECK(all[i].widest_span_min <= 36 + 1000 + 30 + elapsed_min);
        CHECK(all[i].widest_span_min * 20 < full_span_min);
    }
    CHECK(scalar(dsn, "SELECT count(*) FROM market_candles_agg WHERE instrument_id = 9101"
                      " AND timeframe = '5m' AND quality_status = 'ok' AND open_time = " +
                          iso(repair_gap)) == 1);
    CHECK(scalar(dsn, "SELECT count(*) FROM market_candles_agg WHERE instrument_id = 9101"
                      " AND timeframe = '5m' AND open_time = " + iso(orphan_gap)) == 0);

    // ---- test 2: consecutive windows the writer committed late are recovered --
    // Only a periodic pass can reach these: they are older than the live
    // recheck, and a fixed recent window would have missed them too. The reach
    // came from key B's writer frontier at the startup pass.
    CAPTURE(lag_first, lag_last, lag_windows);
    CHECK(scalar(dsn, "SELECT count(*) FROM market_candles_agg WHERE instrument_id = 9102"
                      " AND timeframe = '5m' AND quality_status = 'ok' AND open_time >= " +
                          iso(lag_first) + " AND open_time < " + iso(lag_last)) == lag_windows);
    int64_t periodic_windows = 0;
    for (size_t i = 1; i < all.size(); ++i) periodic_windows += all[i].windows;
    CHECK(periodic_windows >= lag_windows);

    // ---- test 3: an old gap is not pulled into a periodic pass ---------------
    CHECK(scalar(dsn, "SELECT count(*) FROM market_candles_agg WHERE instrument_id = 9101"
                      " AND timeframe = '5m' AND open_time = " + iso(old_gap_2)) == 0);

    // ---- test 5: an empty aggregate key is built from the floor, not 'epoch' -
    CHECK(scalar(dsn, "SELECT count(*) FROM market_candles_agg WHERE instrument_id = 9103") > 0);
    CHECK(scalar(dsn, "SELECT (extract(epoch FROM min(open_time)) * 1000)::bigint"
                      " FROM market_candles_agg WHERE instrument_id = 9103") >= floor);

    // ---- tests 6 and 7: a repair below the floor writes nothing below it ------
    CHECK(occurrences(log, "\"event\":\"agg_recompute_below_partition_floor\"") >= 1);
    CHECK(occurrences(log, "\"event\":\"agg_recomputed_after_repair\"") >= 1);   // the straddling one
    CHECK(scalar(dsn, "SELECT count(*) FROM market_candles_agg WHERE open_time < " + iso(floor)) == 0);

    // ---- tests 8 and 9: history below the floor never reaches the writer ------
    CHECK(occurrences(log, "writer_batch_failed_isolating") == 0);
    CHECK(occurrences(log, "writer_poison_message_rejected") == 0);
    CHECK(occurrences(log, "no partition of relation") == 0);
    CHECK(scalar(dsn, "SELECT count(*) FROM rejected_market_events") == 0);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
