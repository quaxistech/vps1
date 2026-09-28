#pragma once

#include "config/config.hpp"
#include "exchanges/market_data_adapter.hpp"
#include "exchanges/rest_client.hpp"
#include "exchanges/venue_stream.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>

namespace altspotter::exchanges::okx {

// Defaults verified against the official v5 docs + live API, 2026-07:
// REST https://www.okx.com (history-candles 100/page); WS public
// wss://ws.okx.com:8443/ws/v5/public (trades/books/tickers/mark-price/
// funding-rate/open-interest), candle channels live on the business endpoint
// wss://ws.okx.com:8443/ws/v5/business. Heartbeat: text "ping" within 30s idle.
// Order book: standard public "books" channel (400 depth, snapshot+update,
// seqId/prevSeqId + crc32 checksum) — no VIP-only channels.
struct OkxConfig {
    std::string rest_base_url = "https://www.okx.com";
    std::string ws_public_url = "wss://ws.okx.com:8443/ws/v5/public";
    std::string ws_business_url = "wss://ws.okx.com:8443/ws/v5/business";
    int32_t connect_timeout_ms = 8000;
    int32_t request_timeout_ms = 10000;
    double rate_limit_per_second = 4.0; // history-candles cap: 20 req / 2 s / IP
    int32_t max_retries = 4;
    int32_t backoff_min_ms = 300;
    int32_t backoff_max_ms = 15000;
    int32_t stale_timeout_sec = 60;
};

OkxConfig parse_config(const YAML::Node& node);

class OkxUsdtSwapMarketDataAdapter final : public ExchangeMarketDataAdapter {
public:
    OkxUsdtSwapMarketDataAdapter(OkxConfig cfg, std::function<bool()> should_stop);

    [[nodiscard]] std::string exchange_code() const override { return "okx"; }

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

    // /market/history-candles. The venue serves 300 per page, not the 100 this
    // was set to -- verified against the live API at 30 and 280 days back, and
    // 400/500 clamp to 300, so 300 is the true ceiling. At 100 a 280-day 1m
    // backfill cost 4032 requests and 16.8 minutes per instrument, three times
    // Bitget's; OKX was the whole load's critical path for no reason.
    static constexpr int kHistoryPageLimit = 300;
    static constexpr int kRecentPageLimit = 300;  // /market/candles
    // A candle window closed longer ago than this is treated as final even when
    // the venue's `confirm` flag says otherwise (see parse_candles).
    static constexpr int64_t kConfirmGraceMs = 120'000;

    // Funding history is served newest-first, 100 per page, paginated backwards
    // on `after`. A single page is only ~33 days at an 8-hour interval, which
    // is why OKX held 36 days against a configured 180.
    static constexpr int kMarkIndexMaxPages = 2000;
    static constexpr int kFundingPageLimit = 100;
    static constexpr int kFundingMaxPages = 30;

    static Result<std::vector<InstrumentSpec>> parse_instruments(const nlohmann::json& data);
    // Candle rows: [ts,o,h,l,c,vol(contracts),volCcy(base),volCcyQuote(quote),confirm].
    static Result<std::vector<Candle>> parse_candles(const nlohmann::json& data, Timeframe tf,
                                                     CandleSource source, int64_t now_ms);

private:
    Result<nlohmann::json> get_data(const std::string& target);

    OkxConfig cfg_;
    std::function<bool()> should_stop_;
    HttpsClient client_;
    TokenBucket bucket_;
};

// Public-endpoint protocol: trades, books, tickers, mark-price, funding-rate,
// open-interest. Trade/OI sizes arrive in contracts — the runtime multiplies
// by the instrument's contract_value (ctVal) to get base units, so the parser
// needs the per-symbol contract values.
class OkxPublicWsProtocol final : public VenueWsProtocol {
public:
    OkxPublicWsProtocol(OkxConfig cfg, std::map<std::string, Decimal> contract_values)
        : cfg_(std::move(cfg)), contract_values_(std::move(contract_values)) {}

    [[nodiscard]] std::string ws_url() const override { return cfg_.ws_public_url; }
    [[nodiscard]] std::vector<std::string> subscribe_frames(
        const std::vector<std::string>& symbols) const override;
    [[nodiscard]] std::vector<std::string> resubscribe_book_frames(
        const std::string& symbol) const override;
    [[nodiscard]] std::string ping_frame() const override { return "ping"; }
    [[nodiscard]] int ping_interval_sec() const override { return 20; }
    bool parse(std::string_view frame, VenueEvents& out) override;

private:
    OkxConfig cfg_;
    std::map<std::string, Decimal> contract_values_;
};

// Business-endpoint protocol: candle1m only.
class OkxBusinessWsProtocol final : public VenueWsProtocol {
public:
    explicit OkxBusinessWsProtocol(OkxConfig cfg) : cfg_(std::move(cfg)) {}

    [[nodiscard]] std::string ws_url() const override { return cfg_.ws_business_url; }
    [[nodiscard]] std::vector<std::string> subscribe_frames(
        const std::vector<std::string>& symbols) const override;
    [[nodiscard]] std::string ping_frame() const override { return "ping"; }
    [[nodiscard]] int ping_interval_sec() const override { return 20; }
    bool parse(std::string_view frame, VenueEvents& out) override;

private:
    OkxConfig cfg_;
};

} // namespace altspotter::exchanges::okx
