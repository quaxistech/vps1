// End-to-end pipeline: temp NATS + temp PostgreSQL + the real altspotter binary
// running only the storage_writer component. Verifies durable consumption,
// ACK-after-commit, duplicate-delivery idempotency, late-candle revisions and
// restart continuity.

#include "test_env.hpp"

#include "messaging/codec.hpp"
#include "messaging/nats_client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>

using namespace altspotter;
using namespace altspotter::test;

namespace {

constexpr int64_t kBase = 1'783'600'440'000 - (1'783'600'440'000 % 60'000);

std::string candle_batch_envelope(int64_t instrument_id, const std::string& symbol,
                                  int64_t from_ms, int count, const char* close,
                                  const std::string& tag) {
    messaging::EnvelopeMeta meta;
    meta.event_type = "bar.batch.backfill";
    meta.instrument_id = instrument_id;
    meta.exchange_symbol = symbol;
    meta.source_service = "itest";
    meta.message_id = "itest:" + symbol + ":" + std::to_string(from_ms) + ":" + tag;
    auto env = messaging::make_envelope(meta);
    auto* batch = env.mutable_candle_batch();
    for (int i = 0; i < count; ++i) {
        Candle c;
        c.timeframe = Timeframe::m1;
        c.open_time_ms = from_ms + static_cast<int64_t>(i) * 60'000;
        c.close_time_ms = c.open_time_ms + 60'000;
        c.open = *Decimal::parse("100.5");
        c.high = *Decimal::parse("101");
        c.low = *Decimal::parse("99.9");
        c.close = *Decimal::parse(close);
        c.base_volume = *Decimal::parse("2.5");
        c.quote_volume = *Decimal::parse("251.25");
        c.is_closed = true;
        c.source = CandleSource::rest_bootstrap;
        c.quality = Quality::ok;
        messaging::to_proto(c, batch->add_candles());
    }
    return messaging::serialize(env);
}

int64_t count_rows(const std::string& dsn, const std::string& where) {
    pqxx::connection conn(dsn);
    pqxx::read_transaction txn(conn);
    return txn.exec1("SELECT count(*) FROM market_candles_1m WHERE " + where)[0].as<int64_t>();
}

}  // namespace

TEST_CASE("writer pipeline end-to-end", "[integration][pipeline]") {
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

    // Unified config for the real binary; only the storage_writer component runs.
    const auto dir = std::filesystem::temp_directory_path() /
                     ("altspotter_pipe_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto common_path = dir / "common.yaml";
    const auto config_path = dir / "altspotter.yaml";
    {
        std::ofstream common(common_path);
        common << "environment: test\nlog_level: info\ntimezone: UTC\ninstance_id: itest\n"
               << "nats_url: " << nats.url() << "\npostgres_dsn: " << db.dsn()
               << "\nshutdown_timeout_sec: 10\n";
        std::ofstream cfg(config_path);
        cfg << "storage_writer:\n  consumer:\n    stream: ALTSPOTTER_BARS\n"
            << "    durable: storage-writer-bars\n    fetch_batch: 64\n    fetch_timeout_ms: 300\n"
            << "  batch_size: 500\n  flush_interval_ms: 200\n  queue_capacity: 2048\n"
            << "  postgres_pool_size: 2\n";
    }

    auto client = messaging::NatsClient::connect(nats.url(), "itest-pub");
    REQUIRE(client.has_value());

    ChildProcess writer(bin, {"run", "--only", "storage_writer", "--config", config_path.string(),
                              "--common", common_path.string()});
    REQUIRE(writer.running());

    // 1) batch of 10 candles lands in PostgreSQL
    REQUIRE((*client)
                ->js_publish("bar.bitget.BTCUSDT.1m.backfill",
                             candle_batch_envelope(1, "BTCUSDT", kBase, 10, "100.7", "a"), "")
                .has_value());
    int64_t rows = 0;
    for (int i = 0; i < 100; ++i) {
        rows = count_rows(db.dsn(), "instrument_id = 1");
        if (rows == 10) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(rows == 10);

    // 2) duplicate delivery (different msg-id, same content) -> no new rows, no revisions
    REQUIRE((*client)
                ->js_publish("bar.bitget.BTCUSDT.1m.backfill",
                             candle_batch_envelope(1, "BTCUSDT", kBase, 10, "100.7", "b"), "")
                .has_value());
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(count_rows(db.dsn(), "instrument_id = 1") == 10);
    CHECK(count_rows(db.dsn(), "instrument_id = 1 AND revision > 0") == 0);

    // 3) late correction -> same rows, revision bumped
    REQUIRE((*client)
                ->js_publish("bar.bitget.BTCUSDT.1m.recon",
                             candle_batch_envelope(1, "BTCUSDT", kBase, 3, "100.9", "c"), "")
                .has_value());
    int64_t revised = 0;
    for (int i = 0; i < 100; ++i) {
        revised = count_rows(db.dsn(), "instrument_id = 1 AND revision = 1");
        if (revised == 3) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(revised == 3);
    CHECK(count_rows(db.dsn(), "instrument_id = 1") == 10);

    // 4) graceful shutdown, then messages published while the writer is down
    writer.terminate(SIGTERM);
    REQUIRE((*client)
                ->js_publish("bar.bitget.ETHUSDT.1m.backfill",
                             candle_batch_envelope(2, "ETHUSDT", kBase, 5, "100.8", "d"), "")
                .has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(count_rows(db.dsn(), "instrument_id = 2") == 0);  // nobody consumed yet

    // 5) restart: durable consumer continues where it left off
    ChildProcess writer2(bin, {"run", "--only", "storage_writer", "--config", config_path.string(),
                               "--common", common_path.string()});
    REQUIRE(writer2.running());
    int64_t eth_rows = 0;
    for (int i = 0; i < 100; ++i) {
        eth_rows = count_rows(db.dsn(), "instrument_id = 2");
        if (eth_rows == 5) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(eth_rows == 5);
    {
        pqxx::connection conn(db.dsn());
        pqxx::read_transaction txn(conn);
        CHECK(txn.exec1("SELECT count(*) FROM rejected_market_events")[0].as<int64_t>() == 0);
    }

    // 6) invalid event -> rejected_market_events, stream not blocked
    REQUIRE((*client)
                ->js_publish("bar.bitget.XRPUSDT.1m.closed", "garbage-not-protobuf", "")
                .has_value());
    int64_t rejected = 0;
    for (int i = 0; i < 100; ++i) {
        pqxx::connection conn(db.dsn());
        pqxx::read_transaction txn(conn);
        rejected = txn.exec1("SELECT count(*) FROM rejected_market_events")[0].as<int64_t>();
        if (rejected == 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(rejected == 1);
    {
        pqxx::connection conn(db.dsn());
        pqxx::read_transaction txn(conn);
        const auto reason = txn.exec1(
            "SELECT reason FROM rejected_market_events ORDER BY received_at DESC LIMIT 1");
        CHECK(reason[0].as<std::string>() == "envelope_parse");
    }

    writer2.terminate(SIGTERM);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
