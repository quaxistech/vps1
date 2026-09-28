#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace altsignalgateway::domain {

// The point-in-time facts the Gateway carries into the FINAL signal but does
// not itself produce. They are read from AltStrategy's immutable context
// snapshot and AltSpotter's instrument registry, both over the same read-only
// connection.
//
// These structs are deliberately free of protobuf and pqxx: the canonicaliser
// is then a pure function of plain data, and every field-by-field rule in
// docs/SIGNAL_CONTRACT.md can be tested without a database.
//
// WHY ENRICH AT ALL. The external consumer must be able to act on a signal
// without querying VPS1 (docs/SIGNAL_CONTRACT.md §"self-sufficiency"). Two
// things it cannot act without are absent from the upstream wire message: the
// venue-executable symbol — the upstream carries an internal token, and on this
// deployment that token is literally "pair-16" — and the venue's tick/lot
// constraints. Neither is computed here; both are looked up in the registry
// that owns them.

struct NamedScalar {
    std::string name;
    double value = 0.0;
};

struct RegimeSnapshot {
    std::string regime;
    std::optional<double> confidence;
    std::optional<double> trend_strength;
    std::optional<double> volatility_regime;
    std::optional<double> liquidity_quality;

    [[nodiscard]] bool empty() const {
        return regime.empty() && !confidence && !trend_strength && !volatility_regime &&
               !liquidity_quality;
    }
};

// AltStrategy's immutable DecisionContext snapshot, addressed by context_id.
struct ContextSnapshot {
    bool present = false;
    std::string absence_reason;

    std::string context_id;
    int64_t context_revision = 0;
    int64_t event_time_ms = 0;
    int64_t available_at_ms = 0;
    int64_t expires_at_ms = 0;

    std::string global_universe_status;
    std::string data_quality;          // 'STRICT_4_OF_4' | 'DEGRADED_3_OF_4' | ...
    uint32_t venue_mask = 0;
    uint32_t missing_venue_mask = 0;
    bool regime_divergence = false;
    std::string feature_manifest_hash;

    std::optional<double> mark_price;
    std::optional<double> spread_bps;
    std::optional<double> funding_rate;
    std::optional<double> fees_bps;
    std::optional<double> top_depth_usdt;
    std::optional<double> liquidity_rank;
    std::optional<double> expected_slippage_bps;

    // Sorted by name by the loader; the canonicaliser asserts the ordering.
    std::vector<NamedScalar> venue_features;
    std::vector<NamedScalar> cross_venue_features;
    std::vector<NamedScalar> reference_features;
    std::vector<NamedScalar> microstructure;

    RegimeSnapshot venue_regime;
    RegimeSnapshot global_regime;
};

// AltSpotter's instrument registry, resolved for the trade's canonical pair on
// the configured execution exchange.
struct InstrumentResolution {
    bool present = false;              // the pair resolved to a venue instrument
    std::string absence_reason;

    std::string exchange;              // "bitget"
    std::string base_asset;
    std::string quote_asset;
    std::string market_type;
    std::string exchange_symbol;       // the executable symbol, e.g. "XRPUSDT"
    int64_t exchange_instrument_id = 0;
    std::string instrument_status;
    bool whitelisted = false;

    // The spec version that was in force at T0, not the newest one.
    bool spec_present = false;
    std::string spec_absence_reason;
    int64_t spec_version = 0;
    int64_t spec_valid_from_ms = 0;
    std::string contract_type;
    std::string settle_asset;
    // Exact decimal strings, carried verbatim: a tick size is not a double.
    std::string tick_size;
    std::string quantity_step;
    std::string min_order_quantity;
    std::string min_notional;
    std::string contract_value;
    std::optional<int32_t> price_precision;
    std::optional<int32_t> quantity_precision;
    std::optional<double> max_leverage;

    int64_t resolved_at_ms = 0;
};

// AltTech's macro-core block as of T0, read from AltTech's own table over a
// read-only foreign connection. Plain data; the canonicaliser projects it.
struct MacroSnapshot {
    bool present = false;
    std::string absence_reason;

    std::string feature_set_name;      // "macro-core"
    int32_t feature_set_version = 0;
    std::string schema_hash;
    std::string method_version;
    int64_t bar_open_time_ms = 0;
    int64_t computed_at_ms = 0;
    int64_t age_ms = 0;                // T0 - bar_open_time_ms
    std::string status;                // AltTech's VectorStatus name
    int32_t feature_count = 0;
    int32_t valid_count = 0;
    std::vector<NamedScalar> values;   // usable positions, sorted by name
    std::vector<std::string> unavailable;   // the rest, sorted by name
};

// --- extended context (queried, never journalled) ---------------------------
//
// Minute-grid facts from AltSpotter's canonical tables at the last minute that
// had closed by T0. One struct per venue row; every scalar is optional because
// the tables store NULL where the venue reported nothing.

struct CandleBar {
    int64_t open_time_ms = 0;
    double open = 0, high = 0, low = 0, close = 0;
    double base_volume = 0, quote_volume = 0;
    int64_t trade_count = 0;
    std::optional<double> taker_buy_base_volume;
    bool is_closed = false;
    std::string quality_status;
};

struct CandleWindow {
    std::string exchange;
    std::string timeframe;             // "1m" | "5m" | "1h"
    int32_t requested_bars = 0;
    std::vector<CandleBar> bars;       // oldest first
    std::string source_table;
};

struct DerivativesSnapshot {
    std::string exchange;
    int64_t open_time_ms = 0;
    int64_t age_ms = 0;
    std::optional<double> mark_price, index_price, funding_rate;
    std::optional<int64_t> next_funding_time_ms;
    std::optional<double> open_interest_base, open_interest_quote;
    std::optional<double> best_bid, best_ask, spread, turnover_24h;
    std::string quality_status;
    std::string source_table;
};

struct BookSnapshot {
    std::string exchange;
    int64_t open_time_ms = 0;
    int64_t age_ms = 0;
    std::optional<double> best_bid, best_ask, mid_price, spread, relative_spread;
    std::optional<double> bid_depth_5, ask_depth_5, bid_depth_10, ask_depth_10;
    std::optional<double> book_imbalance_5, book_imbalance_10, microprice;
    int64_t snapshot_count = 0;
    std::string quality_status;
    std::string source_table;
};

struct FlowSnapshot {
    std::string exchange;
    int64_t open_time_ms = 0;
    int64_t age_ms = 0;
    int64_t trade_count = 0, buy_trade_count = 0, sell_trade_count = 0;
    std::optional<double> base_volume, quote_volume, aggressive_buy_volume,
        aggressive_sell_volume, signed_volume, vwap, largest_trade;
    std::string quality_status;
    std::string source_table;
};

struct ExtendedContext {
    int64_t t0_ms = 0;
    int64_t read_at_ms = 0;
    MacroSnapshot macro;
    std::vector<DerivativesSnapshot> derivatives;
    std::vector<BookSnapshot> book;
    std::vector<FlowSnapshot> flow;
    std::vector<CandleWindow> candles;
    std::vector<std::string> absent;   // "<group>[:<detail>]:<reason>"
};

// Transport facts the Gateway itself owns.
struct IngestMetadata {
    std::string source_subject;
    std::string source_stream;
    uint64_t source_stream_sequence = 0;
    int64_t received_at_ms = 0;
    int64_t journaled_at_ms = 0;
    int64_t sequence = 0;
    std::string message_id;
    std::string gateway_version;
};

} // namespace altsignalgateway::domain
