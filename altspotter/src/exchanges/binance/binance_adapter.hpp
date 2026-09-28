#pragma once

#include "config/config.hpp"
#include "exchanges/market_data_adapter.hpp"
#include "exchanges/rest_client.hpp"
#include "exchanges/venue_stream.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>

namespace altspotter::exchanges::binance {

// Venue configuration (defaults verified against the official USDS-M docs and
// the live API, 2026-07: REST https://fapi.binance.com, 2400 weight/min/IP;
// WS base URLs are split by stream group — /market carries aggTrade/kline/
// markPrice/ticker while depth flows only on /public (a /market connection
// acks a depth SUBSCRIBE but never delivers it), so the runtime keeps one
// connection per group; 24h connection lifetime, server pings, <=1024 streams).
struct BinanceConfig {
    std::string rest_base_url = "https://fapi.binance.com";
    std::string ws_url = "wss://fstream.binance.com/market/stream";
    std::string ws_public_url = "wss://fstream.binance.com/public/stream";
    int32_t connect_timeout_ms = 8000;
    int32_t request_timeout_ms = 10000;
    double rate_limit_per_second = 2.0; // weight discipline: klines limit=1500 and
                                        // depth limit=500 each cost 10 of 2400/min
    int32_t max_retries = 4;
    int32_t backoff_min_ms = 300;
    int32_t backoff_max_ms = 15000;
    int32_t stale_timeout_sec = 60;
    // Depth-snapshot request weight scales with the limit (500 -> 10, 1000 ->
    // 20 of the 2400/min/IP budget). Only the top levels are ever stored, so
    // 500 keeps the local book correct with a large margin at half the cost.
    int32_t depth_snapshot_limit = 500;
};

BinanceConfig parse_config(const YAML::Node& node);

// REST adapter: /fapi/v1 exchangeInfo, klines (max 1500, ascending),
// fundingRate, openInterest, premiumIndex, depth.
class BinanceUsdmMarketDataAdapter final : public ExchangeMarketDataAdapter {
public:
    BinanceUsdmMarketDataAdapter(BinanceConfig cfg, std::function<bool()> should_stop);

    [[nodiscard]] std::string exchange_code() const override { return "binance_usdm"; }

    Result<std::vector<InstrumentSpec>> list_instruments() override;
    Result<InstrumentSpec> get_instrument_spec(const std::string& exchange_symbol) override;
    Result<CandlePage> fetch_candles(const std::string& exchange_symbol, Timeframe tf,
                                     int64_t start_ms, int64_t end_ms,
                                     CandleQueryKind kind) override;
    [[nodiscard]] Error map_exchange_error(int http_status, const std::string& body) const override;

    Result<std::vector<FundingSettlement>> fetch_funding_history(
        const std::string& exchange_symbol, int64_t from_ms, int64_t to_ms) override;
    Result<std::vector<MarkIndexSample>> fetch_mark_index_history(
        const std::string& exchange_symbol, int64_t from_ms, int64_t to_ms) override;
    Result<OpenInterestSample> fetch_open_interest(const std::string& exchange_symbol) override;
    Result<BookUpdate> fetch_orderbook_snapshot(const std::string& exchange_symbol,
                                                int depth) override;

    static constexpr int kKlinePageLimit = 1500;
    static constexpr int kMarkIndexMaxPages = 600;

    // Exposed for unit tests (parse REST payloads without network).
    static Result<std::vector<InstrumentSpec>> parse_exchange_info(const nlohmann::json& doc);
    static Result<std::vector<Candle>> parse_klines(const nlohmann::json& payload, Timeframe tf,
                                                    CandleSource source, int64_t now_ms);

private:
    Result<nlohmann::json> get_json(const std::string& target);

    BinanceConfig cfg_;
    std::function<bool()> should_stop_;
    HttpsClient client_;
    TokenBucket bucket_;
};

// WS protocol: combined streams, one instance per stream group.
//   market — <s>@aggTrade, <s>@kline_1m, <s>@markPrice (3s), <s>@ticker;
//   depth  — <s>@depth (250ms diff), delivered only by the /public endpoint.
// Protocol-level ping/pong is handled by Beast; rotation before the 24h
// forced disconnect.
class BinanceWsProtocol final : public VenueWsProtocol {
public:
    enum class StreamGroup { market, depth };

    explicit BinanceWsProtocol(BinanceConfig cfg, StreamGroup group = StreamGroup::market)
        : cfg_(std::move(cfg)), group_(group) {}

    [[nodiscard]] std::string ws_url() const override;
    [[nodiscard]] std::vector<std::string> subscribe_frames(
        const std::vector<std::string>& symbols) const override;
    [[nodiscard]] std::string ping_frame() const override { return {}; }
    [[nodiscard]] int ping_interval_sec() const override { return 0; }
    [[nodiscard]] int rotation_interval_sec() const override { return 23 * 3600; }
    bool parse(std::string_view frame, VenueEvents& out) override;

private:
    BinanceConfig cfg_;
    StreamGroup group_;
    mutable int sub_id_ = 0;
};

} // namespace altspotter::exchanges::binance
