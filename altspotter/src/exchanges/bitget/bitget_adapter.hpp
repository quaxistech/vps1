#pragma once

#include "config/config.hpp"
#include "exchanges/market_data_adapter.hpp"
#include "exchanges/rest_client.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>

namespace altspotter::exchanges::bitget {

// All Bitget REST specifics: V2 endpoints, JSON field names, product type,
// pagination caps, business error mapping. See docs/BITGET.md.
class BitgetMarketDataAdapter final : public ExchangeMarketDataAdapter {
public:
    BitgetMarketDataAdapter(config::BitgetRestConfig cfg, std::function<bool()> should_stop);

    [[nodiscard]] std::string exchange_code() const override { return "bitget"; }

    Result<std::vector<InstrumentSpec>> list_instruments() override;
    Result<InstrumentSpec> get_instrument_spec(const std::string& exchange_symbol) override;
    Result<CandlePage> fetch_candles(const std::string& exchange_symbol, Timeframe tf,
                                     int64_t start_ms, int64_t end_ms,
                                     CandleQueryKind kind) override;
    Result<std::vector<FundingSettlement>> fetch_funding_history(
        const std::string& exchange_symbol, int64_t from_ms, int64_t to_ms) override;
    Result<std::vector<MarkIndexSample>> fetch_mark_index_history(
        const std::string& exchange_symbol, int64_t from_ms, int64_t to_ms) override;
    [[nodiscard]] Error map_exchange_error(int http_status, const std::string& body) const override;

    // Page caps per Bitget V2 docs.
    static constexpr int kHistoryPageLimit = 200; // /history-candles
    static constexpr int kRecentPageLimit = 1000; // /candles
    static constexpr int kFundingPageLimit = 100; // /history-fund-rate (pageSize)
    // Bitget paginates funding history by page number, newest first, with no
    // time filter. Walk back until the page predates `from_ms`; this caps the
    // walk so a wide window can never spin against the venue.
    static constexpr int kFundingMaxPages = 20;
    // Mark/index walk back one 1m page at a time, like the candle backfill.
    static constexpr int kMarkIndexMaxPages = 4000;

    // Bitget rejects a startTime/endTime pair spanning more than 90 days with
    // `code=40017 Parameter verification failed`. This is a QUERY-RANGE limit,
    // not a history limit: paginating backwards on endTime alone reaches at
    // least 1080 daily bars (earliest 2023-09-15) and 400 days of 1m. Clamping
    // the requested window keeps deep backfills walking instead of failing on
    // their first call.
    static constexpr int64_t kMaxQuerySpanMs = 90LL * 24 * 60 * 60 * 1000;

    // Start bound for one page of a backwards walk, exposed for unit tests.
    // Returns the startTime to send alongside `end_ms`, clamped so the request
    // never exceeds one page or the venue's 90-day query range.
    [[nodiscard]] static int64_t page_window_start(int64_t start_ms, int64_t end_ms, int limit,
                                                   int64_t span_ms) noexcept;

    // Exposed for unit tests (parses REST payloads without network).
    static Result<std::vector<InstrumentSpec>> parse_contracts(const nlohmann::json& payload,
                                                               const std::string& product_type);
    static Result<std::vector<Candle>> parse_candles(const nlohmann::json& payload,
                                                     Timeframe tf, CandleSource source,
                                                     int64_t now_ms);

private:
    Result<nlohmann::json> get_json(const std::string& target);

    config::BitgetRestConfig cfg_;
    std::function<bool()> should_stop_;
    HttpsClient client_;
    TokenBucket bucket_;
};

} // namespace altspotter::exchanges::bitget
