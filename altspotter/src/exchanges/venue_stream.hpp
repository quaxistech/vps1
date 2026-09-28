#pragma once

#include "core/types.hpp"
#include "exchanges/order_book.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace altspotter::exchanges {

// ---------------------------------------------------------------------------
// Venue-neutral events produced by a VenueWsProtocol parser. Symbols are the
// venue's native symbols (BTCUSDT / BTC-USDT-SWAP); the runtime maps them to
// instrument ids.
// ---------------------------------------------------------------------------

struct VenueTradeEvent {
    std::string symbol;
    Trade trade;              // size already normalized to base units
};

struct VenueCandleEvent {
    std::string symbol;
    Candle candle;            // is_closed set from the venue confirm flag
};

// Consolidated ticker/derivatives-context update; every field optional —
// venues deliver different subsets on different channels.
struct VenueTickerEvent {
    std::string symbol;
    int64_t exchange_ms = 0;
    std::optional<Decimal> last_price;
    std::optional<Decimal> open_24h;
    std::optional<Decimal> high_24h;
    std::optional<Decimal> low_24h;
    std::optional<Decimal> base_volume_24h;
    std::optional<Decimal> quote_volume_24h;
    std::optional<Decimal> mark_price;
    std::optional<Decimal> index_price;
    std::optional<Decimal> funding_rate;
    int64_t next_funding_time_ms = 0;
    std::optional<Decimal> open_interest_base;
    std::optional<Decimal> open_interest_quote;
    std::optional<Decimal> bid_price;
    std::optional<Decimal> bid_size;
    std::optional<Decimal> ask_price;
    std::optional<Decimal> ask_size;
};

struct VenueBookEvent {
    std::string symbol;
    BookUpdate update;
};

struct VenueEvents {
    std::vector<VenueTradeEvent> trades;
    std::vector<VenueCandleEvent> candles;
    std::vector<VenueTickerEvent> tickers;
    std::vector<VenueBookEvent> books;
    bool is_pong = false;
    std::optional<std::string> protocol_error; // subscribe rejection etc.
};

// ---------------------------------------------------------------------------
// Per-venue WS protocol strategy: URLs, subscription frames, heartbeat and
// frame parsing. One protocol instance per WS session; parse() is called on
// the session's io thread only.
// ---------------------------------------------------------------------------
class VenueWsProtocol {
public:
    virtual ~VenueWsProtocol() = default;

    [[nodiscard]] virtual std::string ws_url() const = 0;

    // Frames sent right after the WS handshake to subscribe `symbols`.
    [[nodiscard]] virtual std::vector<std::string> subscribe_frames(
        const std::vector<std::string>& symbols) const = 0;

    // Frames that force a fresh book snapshot for one symbol (Bybit/OKX
    // resync). Empty when the venue uses REST snapshots instead (Binance).
    [[nodiscard]] virtual std::vector<std::string> resubscribe_book_frames(
        const std::string& symbol) const {
        (void)symbol;
        return {};
    }

    // Application-level heartbeat ("" = the venue relies on protocol-level
    // ping/pong frames only, which Boost.Beast answers automatically).
    [[nodiscard]] virtual std::string ping_frame() const = 0;
    [[nodiscard]] virtual int ping_interval_sec() const = 0;

    // Forced connection rotation (seconds; 0 = none). Binance disconnects at
    // 24h — rotate proactively before that.
    [[nodiscard]] virtual int rotation_interval_sec() const { return 0; }

    // Parse one text frame into venue-neutral events. Returning false marks
    // the frame unparseable (logged, dropped).
    virtual bool parse(std::string_view frame, VenueEvents& out) = 0;
};

} // namespace altspotter::exchanges
