#pragma once

#include "core/error.hpp"
#include "core/types.hpp"

#include "system.pb.h"

#include <string>
#include <string_view>

namespace altspotter::messaging {

inline constexpr uint32_t kSchemaVersion = 1;
inline constexpr std::string_view kExchangeBitget = "bitget";

// --- domain <-> proto conversions (exact decimal transport) ---
altspotter::v1::Decimal to_proto(const Decimal& d);
Decimal from_proto(const altspotter::v1::Decimal& d);

void to_proto(const Candle& c, altspotter::v1::Candle* out);
Result<Candle> candle_from_proto(const altspotter::v1::Candle& p);

void to_proto(const Trade& t, altspotter::v1::Trade* out);
Result<Trade> trade_from_proto(const altspotter::v1::Trade& p);

void to_proto(const InstrumentSpec& s, int64_t instrument_id, const std::string& normalized_hash,
              int64_t spec_version, altspotter::v1::InstrumentSpec* out, int16_t exchange_id = 1);

// --- envelope ---
struct EnvelopeMeta {
    std::string message_id;
    std::string event_type;
    int16_t exchange_id = 1; // proto Exchange enum value (1 = bitget, default)
    int64_t instrument_id = 0;
    std::string exchange_symbol;
    int64_t exchange_timestamp_ms = 0;
    std::string trace_id;
    std::string source_service;
    // Multi-venue additions (all optional; zero/empty = unset).
    std::string connection_id;
    std::string source_channel;
    int64_t sequence = 0;
    int64_t local_receive_ms = 0;
};

// Creates an envelope with common fields filled; caller sets the payload oneof.
altspotter::v1::Envelope make_envelope(const EnvelopeMeta& meta);

std::string serialize(const altspotter::v1::Envelope& env);

Result<altspotter::v1::Envelope> parse_envelope(std::string_view bytes);

// Deterministic message ids (JetStream dedup keys).
std::string candle_msg_id(std::string_view exchange, std::string_view symbol, Timeframe tf,
                          int64_t open_time_ms);

} // namespace altspotter::messaging
