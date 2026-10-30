#include "persistence/candle_store.hpp"
#include "test_env.hpp"
#include <catch2/catch_test_macros.hpp>
#include <functional>
using namespace altspotter;
using namespace altspotter::persistence;

TEST_CASE("candle corrections compare effective values with exact revision statistics") {
    test::TempDb database;
    if (!database.create()) SKIP("isolated PostgreSQL DSN required");
    pqxx::connection connection(database.dsn());
    pqxx::work tx(connection);
    tx.exec("CREATE TEMP TABLE market_candles_1m (exchange_id smallint, instrument_id bigint, "
        "open_time timestamptz, close_time timestamptz, open numeric, high numeric, low numeric, "
        "close numeric, base_volume numeric, quote_volume numeric, trade_count bigint, "
        "taker_buy_base_volume numeric, taker_sell_base_volume numeric, is_closed boolean, "
        "source text, quality_status text, exchange_received_at timestamptz, "
        "revision bigint DEFAULT 0, updated_at timestamptz DEFAULT now(), "
        "PRIMARY KEY(exchange_id,instrument_id,open_time))");
    tx.exec("CREATE TEMP TABLE market_candles_agg (LIKE market_candles_1m INCLUDING DEFAULTS, "
        "timeframe text, minute_count bigint, PRIMARY KEY(exchange_id,instrument_id,timeframe,open_time))");
    for (const bool aggregate : {false, true}) {
        const auto upsert = [&](const CandleRow& r) {
            auto result = aggregate ? upsert_candles_agg(tx, {r}) : upsert_candles_1m(tx, {r});
            REQUIRE(result);
            return *result;
        };
        CandleRow original;
        original.exchange_id = 1; original.instrument_id = 1;
        auto& c = original.candle;
        c.timeframe = aggregate ? Timeframe::m5 : Timeframe::m1;
        c.open_time_ms = 1'800'000'000'000; c.close_time_ms = c.open_time_ms + tf_ms(c.timeframe);
        c.open = c.high = c.low = c.close = Decimal(100,0);
        c.base_volume = Decimal(10,0); c.quote_volume = Decimal(1000,0);
        c.source = CandleSource::ws; c.is_closed = true;
        CHECK(upsert(original).inserted == 1);
        CHECK(upsert(original).identical == 1);
        std::vector<std::function<void(Candle&)>> corrections{
            [](Candle& x) { x.trade_count = 5; },
            [](Candle& x) { x.trade_count = 6; },
            [](Candle& x) { x.taker_buy_base_volume = Decimal(3,0); },
            [](Candle& x) { x.taker_buy_base_volume = Decimal(4,0); },
            [](Candle& x) { x.taker_sell_base_volume = Decimal(7,0); },
            [](Candle& x) { x.taker_sell_base_volume = Decimal(8,0); },
            [](Candle& x) { x.quality = Quality::suspect; },
            [](Candle& x) { x.source = CandleSource::rest_reconcile; },
            [](Candle& x) { x.is_closed = false; },
            [](Candle& x) { x.close_time_ms += 1; }};
        if (aggregate) {
            corrections.push_back([](Candle& x) { x.minute_count = 4; });
            corrections.push_back([](Candle& x) { x.minute_count = 5; });
        }
        for (const auto& correct : corrections) {
            correct(c);
            const auto result = upsert(original);
            CHECK(result.updated == 1); CHECK(result.inserted == 0); CHECK(result.identical == 0);
            CHECK(upsert(original).identical == 1);
        }
        auto missing = original;
        missing.candle.trade_count.reset(); missing.candle.taker_buy_base_volume.reset();
        missing.candle.taker_sell_base_volume.reset(); missing.candle.minute_count.reset();
        missing.exchange_received_at_ms = c.open_time_ms + 999'999;
        CHECK(upsert(missing).identical == 1);
        // Missing optional values must also survive an UPDATE triggered by a
        // different field, not only the identical-row no-op path.
        missing.candle.quality = Quality::ok;
        CHECK(upsert(missing).updated == 1);
        CHECK(upsert(missing).identical == 1);
        const auto table = aggregate ? "market_candles_agg" : "market_candles_1m";
        const auto row = tx.exec(std::string("SELECT revision,trade_count,taker_buy_base_volume, ") +
                                "taker_sell_base_volume" + (aggregate ? ",minute_count" : "") +
                                " FROM " + table)[0];
        CHECK(row[0].as<int64_t>() == static_cast<int64_t>(corrections.size()+1));
        CHECK(row[1].as<int>() == 6);
        CHECK(row[2].as<int>() == 4); CHECK(row[3].as<int>() == 8);
        if (aggregate) CHECK(row[4].as<int>() == 5);
    }
}
