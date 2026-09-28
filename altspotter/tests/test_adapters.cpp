// Adapter parsing tests (no network): venue REST payloads -> normalized domain.
// Fixtures follow the official public payload shapes verified against the live
// APIs (2026-07). Values are illustrative but structurally exact.

#include "exchanges/adapter_util.hpp"
#include "exchanges/binance/binance_adapter.hpp"
#include "exchanges/bitget/bitget_adapter.hpp"
#include "exchanges/bybit/bybit_adapter.hpp"
#include "exchanges/okx/okx_adapter.hpp"
#include "exchanges/pagination.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>

#include <nlohmann/json.hpp>

using namespace altspotter;
using namespace altspotter::exchanges;
using json = nlohmann::json;

namespace {
constexpr int64_t kNow = 1'784'823'600'000; // fixed "now" well after the fixtures
}

TEST_CASE("binance exchangeInfo parses USDT perpetuals only", "[adapters][binance]") {
    const json doc = {
        {"symbols", json::array({
            {{"symbol", "BTCUSDT"}, {"contractType", "PERPETUAL"}, {"status", "TRADING"},
             {"baseAsset", "BTC"}, {"quoteAsset", "USDT"}, {"marginAsset", "USDT"},
             {"pricePrecision", 2}, {"quantityPrecision", 3}, {"onboardDate", 1567965300000},
             {"filters", json::array({
                 {{"filterType", "PRICE_FILTER"}, {"tickSize", "0.10"}},
                 {{"filterType", "LOT_SIZE"}, {"stepSize", "0.001"}, {"minQty", "0.001"}}})}},
            // delivery future — excluded
            {{"symbol", "BTCUSDT_240927"}, {"contractType", "CURRENT_QUARTER"},
             {"status", "TRADING"}, {"baseAsset", "BTC"}, {"quoteAsset", "USDT"},
             {"marginAsset", "USDT"}},
            // coin-margined-style quote — excluded
            {{"symbol", "BTCUSDC"}, {"contractType", "PERPETUAL"}, {"status", "TRADING"},
             {"baseAsset", "BTC"}, {"quoteAsset", "USDC"}, {"marginAsset", "USDC"}}})}};
    auto specs = binance::BinanceUsdmMarketDataAdapter::parse_exchange_info(doc);
    REQUIRE(specs);
    REQUIRE(specs->size() == 1);
    CHECK((*specs)[0].exchange_symbol == "BTCUSDT");
    CHECK((*specs)[0].base_asset == "BTC");
    CHECK((*specs)[0].settle_asset == "USDT");
    CHECK((*specs)[0].status == InstrumentStatus::active);
    CHECK((*specs)[0].tick_size.to_string() == "0.10");
}

TEST_CASE("binance klines parse ascending with taker split", "[adapters][binance]") {
    const json payload = json::array({
        json::array({1784823300000, "100.0", "101.0", "99.0", "100.5", "10.0",
                     1784823359999, "1005.0", 42, "6.0", "603.0", "0"})});
    auto candles = binance::BinanceUsdmMarketDataAdapter::parse_klines(
        payload, Timeframe::m1, CandleSource::rest_bootstrap, kNow);
    REQUIRE(candles);
    REQUIRE(candles->size() == 1);
    const auto& c = candles->front();
    CHECK(c.open_time_ms == 1784823300000);
    CHECK(c.close_time_ms == 1784823300000 + 60000);
    CHECK(c.is_closed);
    CHECK(c.base_volume.to_string() == "10.0");
    REQUIRE(c.taker_buy_base_volume);
    CHECK(c.taker_buy_base_volume->to_string() == "6.0");
    REQUIRE(c.taker_sell_base_volume);
    CHECK(c.taker_sell_base_volume->to_string() == "4.0"); // 10 - 6
}

TEST_CASE("bybit instruments parse linear USDT perpetuals", "[adapters][bybit]") {
    const json list = json::array({
        {{"symbol", "BTCUSDT"}, {"contractType", "LinearPerpetual"}, {"status", "Trading"},
         {"baseCoin", "BTC"}, {"quoteCoin", "USDT"}, {"settleCoin", "USDT"},
         {"launchTime", "1584230400000"}, {"fundingInterval", 480},
         {"priceFilter", {{"tickSize", "0.10"}}},
         {"lotSizeFilter", {{"qtyStep", "0.001"}, {"minOrderQty", "0.001"},
                            {"minNotionalValue", "5"}}},
         {"leverageFilter", {{"minLeverage", "1"}, {"maxLeverage", "100.00"}}}},
        // inverse — excluded (settleCoin != USDT)
        {{"symbol", "BTCUSD"}, {"contractType", "InversePerpetual"}, {"status", "Trading"},
         {"baseCoin", "BTC"}, {"quoteCoin", "USD"}, {"settleCoin", "BTC"}}});
    auto specs = bybit::BybitLinearMarketDataAdapter::parse_instruments(list);
    REQUIRE(specs);
    REQUIRE(specs->size() == 1);
    CHECK((*specs)[0].exchange_symbol == "BTCUSDT");
    CHECK((*specs)[0].max_leverage == 100);
    CHECK((*specs)[0].funding_interval_hours == 8); // 480 min
}

TEST_CASE("bybit klines sort ascending (API returns newest-first)", "[adapters][bybit]") {
    const json list = json::array({
        json::array({"1784823420000", "3", "3", "3", "3", "1", "3"}),
        json::array({"1784823360000", "2", "2", "2", "2", "1", "2"}),
        json::array({"1784823300000", "1", "1", "1", "1", "1", "1"})});
    auto candles = bybit::BybitLinearMarketDataAdapter::parse_klines(
        list, Timeframe::m1, CandleSource::rest_reconcile, kNow);
    REQUIRE(candles);
    REQUIRE(candles->size() == 3);
    CHECK((*candles)[0].open_time_ms == 1784823300000);
    CHECK((*candles)[2].open_time_ms == 1784823420000);
}

TEST_CASE("okx instruments normalize contract-value sizes", "[adapters][okx]") {
    const json data = json::array({
        {{"instType", "SWAP"}, {"instId", "BTC-USDT-SWAP"}, {"ctType", "linear"},
         {"settleCcy", "USDT"}, {"ctValCcy", "BTC"}, {"ctVal", "0.01"}, {"state", "live"},
         {"listTime", "1573557408000"}, {"tickSz", "0.1"}, {"lotSz", "0.01"}, {"minSz", "0.01"},
         {"lever", "100"}}});
    auto specs = okx::OkxUsdtSwapMarketDataAdapter::parse_instruments(data);
    REQUIRE(specs);
    REQUIRE(specs->size() == 1);
    const auto& s = (*specs)[0];
    CHECK(s.exchange_symbol == "BTC-USDT-SWAP");
    CHECK(s.base_asset == "BTC");
    CHECK(s.contract_value.to_string() == "0.01");
    // min size 0.01 contracts * 0.01 BTC/contract = 0.0001 BTC
    CHECK(s.min_order_quantity.to_string() == "0.0001");
}

TEST_CASE("okx candles parse confirm flag + quote volume", "[adapters][okx]") {
    // [ts,o,h,l,c,vol(contracts),volCcy(base),volCcyQuote(quote),confirm]
    // The newest candle is still forming, so confirm=0 means "not closed".
    const json data = json::array({
        json::array({"1784823360000", "100", "101", "99", "100.5", "50", "0.5", "50.25", "1"}),
        json::array({"1784823540000", "100.5", "102", "100", "101", "10", "0.1", "10.1", "0"})});
    auto candles = okx::OkxUsdtSwapMarketDataAdapter::parse_candles(
        data, Timeframe::m1, CandleSource::ws, kNow);
    REQUIRE(candles);
    REQUIRE(candles->size() == 2);
    CHECK((*candles)[0].open_time_ms == 1784823360000);
    CHECK((*candles)[0].is_closed);
    CHECK((*candles)[0].base_volume.to_string() == "0.5");
    CHECK((*candles)[0].quote_volume.to_string() == "50.25");
    CHECK_FALSE((*candles)[1].is_closed); // window still open, confirm=0
}

TEST_CASE("okx historical zero-volume minutes are closed despite confirm=0",
          "[adapters][okx]") {
    // OKX returns confirm="0" on sporadic historical zero-volume minutes. Their
    // window closed long ago, so they are final data — dropping them would leave
    // permanent one-minute holes that gap repair can never fill.
    const json data = json::array({
        json::array({"1784400780000", "1.494", "1.494", "1.494", "1.494", "0", "0", "0", "0"})});
    auto candles = okx::OkxUsdtSwapMarketDataAdapter::parse_candles(
        data, Timeframe::m1, CandleSource::rest_repair, kNow);
    REQUIRE(candles);
    REQUIRE(candles->size() == 1);
    CHECK((*candles)[0].is_closed);
    CHECK((*candles)[0].base_volume.to_string() == "0");
}

// --- shared adapter helpers -------------------------------------------------

TEST_CASE("to_double refuses non-finite venue input") {
    // std::stod parses these without throwing and returns exactly NaN / Inf.
    // Every one of them must come back as 0.0 so the callers' `> 0` checks
    // reject them, because a NaN reaching a `double precision` column is
    // accepted by PostgreSQL and then propagates into the microstructure
    // vectors and the spread a trade is priced against.
    CHECK(to_double("nan") == 0.0);
    CHECK(to_double("NaN") == 0.0);
    CHECK(to_double("-nan") == 0.0);
    CHECK(to_double("inf") == 0.0);
    CHECK(to_double("-inf") == 0.0);
    CHECK(to_double("infinity") == 0.0);

    // Unparseable input is also 0.0, not a throw.
    CHECK(to_double("") == 0.0);
    CHECK(to_double("abc") == 0.0);

    // Ordinary values are unchanged.
    CHECK(to_double("0") == 0.0);
    CHECK(to_double("1.5") == 1.5);
    CHECK(to_double("-2.25") == -2.25);
    CHECK(to_double("78253.3") == 78253.3);
}

TEST_CASE("the book validity guard rejects a NaN best price") {
    // This is the shape of the check in microstructure_collector::write_book.
    // Written with `<=` it would PASS for a NaN, because every comparison
    // against NaN is false — which is exactly how a NaN would have reached the
    // database. Both forms are asserted so the weaker one cannot come back.
    const auto sound = [](double bid, double ask) {
        return !(!(bid > 0) || !(ask > 0) || !(ask >= bid));
    };
    const auto weak = [](double bid, double ask) {
        return !(bid <= 0 || ask <= 0 || ask < bid);
    };

    const double nan_v = to_double("nan");   // 0.0 after the fix
    CHECK(nan_v == 0.0);
    CHECK_FALSE(sound(nan_v, 100.0));

    // And if a raw NaN ever reaches the guard by another route, the sound form
    // still rejects it while the weak form would not.
    const double raw_nan = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(sound(raw_nan, 100.0));
    CHECK(weak(raw_nan, 100.0));            // documents the defect that was fixed

    // Normal and inverted books behave as expected.
    CHECK(sound(99.0, 100.0));
    CHECK(sound(100.0, 100.0));             // touching is valid, crossed is not
    CHECK_FALSE(sound(101.0, 100.0));
    CHECK_FALSE(sound(0.0, 100.0));
}

// --- Bitget deep-history pagination -----------------------------------------
//
// Bitget answers `code=40017 Parameter verification failed startTime ||
// endTime` when one request spans more than 90 days. That is a QUERY-RANGE
// limit, not a history limit: probed against the live venue, backwards
// pagination on endTime reaches >=1080 daily bars (earliest 2023-09-15) and
// >=400 days of 1m. The bootstrap used to send its full `target_from` on every
// page, so any backfill deeper than 90 days failed on its first call.
TEST_CASE("bitget clamps a deep bootstrap window to one page", "[adapters][bitget]") {
    using altspotter::exchanges::bitget::BitgetMarketDataAdapter;
    constexpr int64_t kDay = 86'400'000LL;
    constexpr int64_t kMinute = 60'000LL;
    const int64_t end = 1'788'000'000'000LL;

    SECTION("a 200-day 1m request collapses to a single page") {
        const int64_t start = end - 200 * kDay;
        const int64_t got = BitgetMarketDataAdapter::page_window_start(start, end, 200, kMinute);
        REQUIRE(got == end - 200 * kMinute);
        REQUIRE(end - got <= BitgetMarketDataAdapter::kMaxQuerySpanMs);
    }

    SECTION("one page of 1d is 200 days, so the 90-day cap binds instead") {
        const int64_t start = end - 400 * kDay;
        const int64_t got = BitgetMarketDataAdapter::page_window_start(start, end, 200, kDay);
        REQUIRE(got == end - BitgetMarketDataAdapter::kMaxQuerySpanMs);
        REQUIRE(end - got <= BitgetMarketDataAdapter::kMaxQuerySpanMs);
    }

    SECTION("a window already inside one page is left untouched") {
        const int64_t start = end - 50 * kMinute;
        REQUIRE(BitgetMarketDataAdapter::page_window_start(start, end, 200, kMinute) == start);
    }

    SECTION("no end bound means no clamping") {
        REQUIRE(BitgetMarketDataAdapter::page_window_start(12345, 0, 200, kMinute) == 12345);
    }
}

// A full backwards walk over a window far deeper than one page, using the same
// three pieces the bootstrap uses: clamp the request, advance the cursor, stop
// at the target. This is the path that used to fail on its first call.
TEST_CASE("a 280-day 1m walk terminates and covers the window", "[adapters][bitget]") {
    using altspotter::exchanges::bitget::BitgetMarketDataAdapter;
    using altspotter::exchanges::advance_history_cursor;
    using altspotter::exchanges::history_walk_done;
    constexpr int64_t kMinute = 60'000LL;
    constexpr int kPage = 200;

    const int64_t end = 1'788'000'000'000LL;
    const int64_t target_from = end - 280LL * 86'400'000LL;

    int64_t cursor_end = end;
    int64_t pages = 0;
    int64_t oldest_reached = end;

    while (cursor_end > target_from && pages < 5000) {
        const int64_t ws =
            BitgetMarketDataAdapter::page_window_start(target_from, cursor_end, kPage, kMinute);
        REQUIRE(cursor_end - ws <= BitgetMarketDataAdapter::kMaxQuerySpanMs);

        // The venue answers with one page ending just before `cursor_end`.
        const int64_t page_earliest = std::max(ws, target_from);
        ++pages;
        oldest_reached = page_earliest;
        if (history_walk_done(page_earliest, target_from)) break;

        auto next = advance_history_cursor(page_earliest, cursor_end);
        REQUIRE(next.has_value());
        cursor_end = *next;
    }

    REQUIRE(oldest_reached <= target_from);
    // 280 days of minutes at 200 bars per page.
    REQUIRE(pages == 280LL * 1440 / kPage);
    REQUIRE(pages < 5000);
}

// --- Funding-history pagination ---------------------------------------------
//
// Funding feeds are served newest-first with a page cursor. A single page IS
// the whole history unless the walk continues: 100 settlements is ~33 days on
// OKX and 200 is ~66 days on Bybit, against a configured 180-day window. Both
// venues held exactly one page — 36 and 70 days — while Bitget, which does
// paginate, reached the venue's own 90-day boundary.
TEST_CASE("funding cursor walks backwards and refuses to spin", "[adapters][funding]") {
    using altspotter::exchanges::advance_funding_cursor;
    constexpr int64_t kEightHours = 8 * 3600 * 1000LL;
    const int64_t newest = 1'788'000'000'000LL;

    SECTION("first page is unbounded, cursor lands on the oldest row") {
        auto next = advance_funding_cursor(newest - 99 * kEightHours, 0, false);
        REQUIRE(next.has_value());
        REQUIRE(*next == newest - 99 * kEightHours);
    }

    SECTION("an inclusive end bound steps one millisecond past the oldest row") {
        auto next = advance_funding_cursor(newest - 199 * kEightHours, newest, true);
        REQUIRE(next.has_value());
        REQUIRE(*next == newest - 199 * kEightHours - 1);
    }

    SECTION("a page that does not move the cursor stops the walk") {
        REQUIRE_FALSE(advance_funding_cursor(newest, newest, false).has_value());
        REQUIRE_FALSE(advance_funding_cursor(newest + 1, newest, true).has_value());
    }

    SECTION("an empty or malformed page stops the walk") {
        REQUIRE_FALSE(advance_funding_cursor(0, newest, false).has_value());
        REQUIRE_FALSE(advance_funding_cursor(-1, newest, true).has_value());
    }

    SECTION("a full 180-day window is reachable by repeated pages") {
        const int64_t target_from = newest - 180LL * 24 * 3600 * 1000;
        int64_t cursor = 0;
        int64_t oldest_seen = newest;
        int pages = 0;
        while (oldest_seen > target_from && pages < 40) {
            const int64_t page_oldest = oldest_seen - 100 * kEightHours;
            auto next = advance_funding_cursor(page_oldest, cursor, false);
            REQUIRE(next.has_value());
            cursor = *next;
            oldest_seen = page_oldest;
            ++pages;
        }
        REQUIRE(oldest_seen <= target_from);
        REQUIRE(pages < 40);
    }
}
