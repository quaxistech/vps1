#pragma once

#include "core/error.hpp"
#include "core/types.hpp"
#include "exchanges/order_book.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace altspotter::exchanges {

// One historical funding settlement (REST backfill; small series).
struct FundingSettlement {
    int64_t funding_time_ms = 0;
    Decimal rate;
    std::optional<Decimal> mark_price;
};

// One historical mark/index observation, aligned to a 1m bar.
//
// Both series are backfillable on all four venues, while open interest is not:
// Bitget serves no historical OI at all and Binance caps at ~30 days. Keeping
// them in one struct matches how they are stored -- two columns of the same
// per-minute derivatives row -- and lets a venue fill only what it serves.
struct MarkIndexSample {
    int64_t open_time_ms = 0;
    std::optional<Decimal> mark_price;
    std::optional<Decimal> index_price;
};

// One open-interest observation (REST poll).
struct OpenInterestSample {
    int64_t ts_ms = 0;
    std::optional<Decimal> base;   // OI in base units
    std::optional<Decimal> quote;  // OI value in USDT
};

// Kind of REST candle query; adapters map this onto exchange-specific endpoints.
enum class CandleQueryKind {
    recent,     // latest closed candles (reconciliation)
    historical, // deep history pagination (bootstrap / gap repair)
};

struct CandlePage {
    std::vector<Candle> candles; // ascending by open_time, closed candles only
    bool exhausted = false;      // exchange confirmed there is no more data in range
};

// Exchange-neutral market data access. The domain layer depends only on this
// interface; Bitget/Binance/OKX/Bybit specifics live in per-exchange adapters.
class ExchangeMarketDataAdapter {
public:
    virtual ~ExchangeMarketDataAdapter() = default;

    [[nodiscard]] virtual std::string exchange_code() const = 0;

    // Full active contract list, normalized (only USDT-margined perpetuals kept).
    virtual Result<std::vector<InstrumentSpec>> list_instruments() = 0;

    // Single instrument spec by exchange symbol.
    virtual Result<InstrumentSpec> get_instrument_spec(const std::string& exchange_symbol) = 0;

    // Closed candles in [start_ms, end_ms), ascending. Number of rows is limited
    // by exchange page caps; callers advance their own cursor.
    virtual Result<CandlePage> fetch_candles(const std::string& exchange_symbol, Timeframe tf,
                                             int64_t start_ms, int64_t end_ms,
                                             CandleQueryKind kind) = 0;

    // Map an exchange-specific error payload into the shared taxonomy.
    [[nodiscard]] virtual Error map_exchange_error(int http_status, const std::string& body) const = 0;

    // ---- optional venue capabilities (default: not supported) ---------------
    // Funding settlement history in [from_ms, to_ms], ascending.
    virtual Result<std::vector<FundingSettlement>> fetch_funding_history(
        const std::string& exchange_symbol, int64_t from_ms, int64_t to_ms) {
        (void)exchange_symbol; (void)from_ms; (void)to_ms;
        return fail(ErrorCode::invalid_argument, "funding history not supported");
    }

    // Historical mark/index prices as 1m bars in [from_ms, to_ms), ascending.
    // Backfilled into market_derivatives_context_1m with source_mask bits 1|2,
    // so a historical row is honestly partial: OI, funding and BBO stay NULL on
    // minutes that were never observed live.
    virtual Result<std::vector<MarkIndexSample>> fetch_mark_index_history(
        const std::string& exchange_symbol, int64_t from_ms, int64_t to_ms) {
        (void)exchange_symbol; (void)from_ms; (void)to_ms;
        return fail(ErrorCode::invalid_argument, "mark/index history not supported");
    }

    // Current open interest.
    virtual Result<OpenInterestSample> fetch_open_interest(const std::string& exchange_symbol) {
        (void)exchange_symbol;
        return fail(ErrorCode::invalid_argument, "open interest not supported");
    }

    // Full-depth REST order-book snapshot (Binance local-book seeding).
    virtual Result<BookUpdate> fetch_orderbook_snapshot(const std::string& exchange_symbol,
                                                        int depth) {
        (void)exchange_symbol; (void)depth;
        return fail(ErrorCode::invalid_argument, "orderbook snapshot not supported");
    }
};

// Live stream callbacks (WebSocket side). Implementations must invoke callbacks
// from a single thread. Timestamps: exchange event time + local receive time.
// One price level of a public order book. Prices/sizes are kept as the exact
// decimal strings the exchange sends so no precision is lost before storage.
struct OrderBookLevel {
    std::string price;
    std::string size;
};

// Public order-book snapshot (top-N depth). Emitted by adapters that subscribe
// to a depth channel; consumers derive imbalance/microprice/spread from it.
struct OrderBook {
    std::string symbol;
    int64_t exchange_ms = 0;         // exchange event time (ms)
    int64_t seq = 0;                 // exchange update sequence, if provided
    std::vector<OrderBookLevel> bids; // best-first (highest price first)
    std::vector<OrderBookLevel> asks; // best-first (lowest price first)
};

struct StreamCallbacks {
    std::function<void(const std::string& symbol, const Trade&, int64_t received_ms,
                       const std::string& connection_id)>
        on_trade;
    std::function<void(const std::string& symbol, const Candle&, int64_t received_ms,
                       const std::string& connection_id)>
        on_candle_update;
    std::function<void(const std::string& symbol, const Candle&, int64_t received_ms,
                       const std::string& connection_id)>
        on_candle_closed;
    // Optional: order-book depth snapshots. Left null by consumers (e.g. the
    // market gateway) that do not need book data, so it is fully additive.
    std::function<void(const std::string& symbol, const OrderBook&, int64_t received_ms,
                       const std::string& connection_id)>
        on_orderbook;
    std::function<void(const std::string& kind, const std::string& connection_id,
                       const std::string& detail, int32_t subscribed)>
        on_connection_event; // kind: connected|disconnected|degraded|recovered
};

// Live market stream contract for the channels the runtime actually consumes.
// Add an order-book callback and subscription only together with its complete
// normalized domain type, persistence/quality policy and downstream consumer;
// a no-op interface method would falsely advertise production capability.
class ExchangeMarketStream {
public:
    virtual ~ExchangeMarketStream() = default;

    virtual void start(const std::vector<std::string>& symbols, StreamCallbacks callbacks) = 0;
    virtual void update_symbols(const std::vector<std::string>& symbols) = 0;
    virtual void stop() = 0;
};

} // namespace altspotter::exchanges
