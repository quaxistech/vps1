#pragma once

#include "alttech/domain/venue.hpp"
// Stable alttech.v1 wire codec used by AltTech producers and consumers.

#include "alttech/common/error.hpp"
#include "alttech/domain/types.hpp"

#include "system.pb.h"

#include <string>
#include <string_view>

namespace alttech::messaging {

inline constexpr uint32_t kSchemaVersion = 1;
inline constexpr std::string_view kExchangeBitget = "bitget";

// --- domain <-> proto conversions (exact decimal transport) ---
::alttech::v1::Decimal to_proto(const Decimal& d);
Decimal from_proto(const ::alttech::v1::Decimal& d);

void to_proto(const Candle& c, ::alttech::v1::Candle* out);
Result<Candle> candle_from_proto(const ::alttech::v1::Candle& p);

void to_proto(const Trade& t, ::alttech::v1::Trade* out);
Result<Trade> trade_from_proto(const ::alttech::v1::Trade& p);

// --- envelope ---
struct EnvelopeMeta {
    std::string message_id;
    std::string event_type;
    int64_t instrument_id = 0;
    std::string exchange_symbol;
    int64_t exchange_timestamp_ms = 0;
    std::string trace_id;
    std::string source_service;
    // Which venue this message is ABOUT.
    //
    // This was not a field: make_envelope() stamped EXCHANGE_BITGET into every
    // envelope AltTech emits. For the primary-venue-only modules that was
    // accidentally correct, but the feature module computes all four venues,
    // so Binance/OKX/Bybit vectors went out labelled Bitget -- and a consumer
    // that reads the venue from the envelope (AltML does) would file them as
    // Bitget. Silent cross-venue contamination, invisible downstream because
    // every field still looked plausible.
    //
    // The default stays the primary venue so the genuinely Bitget-only
    // publishers (regimes, structure, status) keep their meaning unchanged.
    Venue venue = kPrimaryVenue;
};

// Creates an envelope with common fields filled; caller sets the payload oneof.
::alttech::v1::Envelope make_envelope(const EnvelopeMeta& meta);

std::string serialize(const ::alttech::v1::Envelope& env);

Result<::alttech::v1::Envelope> parse_envelope(std::string_view bytes);

// Deterministic message ids (JetStream dedup keys).
std::string candle_msg_id(std::string_view exchange, std::string_view symbol, Timeframe tf,
                          int64_t open_time_ms);

} // namespace alttech::messaging
