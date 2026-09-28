#pragma once

#include "config/config.hpp"
#include "exchanges/market_data_adapter.hpp"
#include "exchanges/rest_client.hpp"
#include "exchanges/venue_stream.hpp"

#include <nlohmann/json.hpp>

#include <functional>

namespace altspotter::exchanges::bybit {

// Defaults verified against the official v5 docs + live API, 2026-07:
// REST https://api.bybit.com (600 req / 5 s / IP shared);
// WS wss://stream.bybit.com/v5/public/linear, {"op":"ping"} every 20s,
// orderbook.{depth} snapshot/delta (u=1 => full reset), tickers snapshot/delta.
struct BybitConfig {
    std::string rest_base_url = "https://api.bybit.com";
    std::string ws_url = "wss://stream.bybit.com/v5/public/linear";
    int32_t connect_timeout_ms = 8000;
    int32_t request_timeout_ms = 10000;
    double rate_limit_per_second = 8.0;
    int32_t max_retries = 4;
    int32_t backoff_min_ms = 300;
    int32_t backoff_max_ms = 15000;
    int32_t stale_timeout_sec = 60;
    int32_t book_depth = 50; // orderbook.{50} topic: 20ms push for linear
};

BybitConfig parse_config(const YAML::Node& node);

class BybitLinearMarketDataAdapter final : public ExchangeMarketDataAdapter {
public:
    BybitLinearMarketDataAdapter(BybitConfig cfg, std::function<bool()> should_stop);

    [[nodiscard]] std::string exchange_code() const override { return "bybit"; }

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

    static constexpr int kKlinePageLimit = 1000;

    // Funding history is newest-first, 200 per page, paginated backwards on
    // `endTime`. One page is ~66 days at an 8-hour interval, which is why Bybit
    // held 70 days against a configured 180.
    static constexpr int kMarkIndexMaxPages = 600;
    static constexpr int kFundingPageLimit = 200;
    static constexpr int kFundingMaxPages = 30;

    static Result<std::vector<InstrumentSpec>> parse_instruments(const nlohmann::json& list);
    static Result<std::vector<Candle>> parse_klines(const nlohmann::json& list, Timeframe tf,
                                                    CandleSource source, int64_t now_ms);

private:
    // GET + v5 envelope check (retCode == 0); returns `result`.
    Result<nlohmann::json> get_result(const std::string& target);

    BybitConfig cfg_;
    std::function<bool()> should_stop_;
    HttpsClient client_;
    TokenBucket bucket_;
};

// WS topics per symbol: publicTrade.S, kline.1.S, orderbook.{depth}.S, tickers.S.
class BybitWsProtocol final : public VenueWsProtocol {
public:
    explicit BybitWsProtocol(BybitConfig cfg) : cfg_(std::move(cfg)) {}

    [[nodiscard]] std::string ws_url() const override { return cfg_.ws_url; }
    [[nodiscard]] std::vector<std::string> subscribe_frames(
        const std::vector<std::string>& symbols) const override;
    [[nodiscard]] std::vector<std::string> resubscribe_book_frames(
        const std::string& symbol) const override;
    [[nodiscard]] std::string ping_frame() const override { return R"({"op":"ping"})"; }
    [[nodiscard]] int ping_interval_sec() const override { return 20; }
    bool parse(std::string_view frame, VenueEvents& out) override;

private:
    BybitConfig cfg_;
};

} // namespace altspotter::exchanges::bybit
