#pragma once

#include "core/types.hpp"

#include <string>
#include <string_view>

namespace altspotter::subjects {

// Core NATS (ephemeral market data)
inline std::string trade(std::string_view exchange, std::string_view symbol) {
    return std::string("md.").append(exchange).append(".").append(symbol).append(".trade");
}

inline std::string candle_update(std::string_view exchange, std::string_view symbol) {
    return std::string("md.").append(exchange).append(".").append(symbol).append(".candle.1m.update");
}

// JetStream ALTSPOTTER_BARS (bar.>)
inline std::string bar_closed(std::string_view exchange, std::string_view symbol, Timeframe tf) {
    return std::string("bar.").append(exchange).append(".").append(symbol).append(".")
        .append(tf_label(tf)).append(".closed");
}

// Batched REST/aggregation write paths; kind: backfill | recon | repair | agg
inline std::string bar_batch(std::string_view exchange, std::string_view symbol, Timeframe tf,
                             std::string_view kind) {
    return std::string("bar.").append(exchange).append(".").append(symbol).append(".")
        .append(tf_label(tf)).append(".").append(kind);
}

// JetStream ALTSPOTTER_CONTROL
inline std::string instrument_event(std::string_view exchange, std::string_view kind) {
    return std::string("evt.instrument.").append(exchange).append(".").append(kind);
}

inline std::string repair_command(std::string_view exchange, std::string_view symbol) {
    return std::string("cmd.history.").append(exchange).append(".").append(symbol).append(".repair");
}

inline std::string repair_result(std::string_view exchange, std::string_view symbol, bool completed) {
    return std::string("evt.history.").append(exchange).append(".").append(symbol)
        .append(completed ? ".repair.completed" : ".repair.failed");
}

inline std::string gateway_event(std::string_view exchange, std::string_view kind) {
    return std::string("evt.gateway.").append(exchange).append(".").append(kind);
}

inline std::string integrity_event(std::string_view exchange, std::string_view symbol,
                                   std::string_view kind) {
    return std::string("evt.integrity.").append(exchange).append(".").append(symbol).append(".")
        .append(kind);
}

// --- Multi-venue market context (JetStream ALTSPOTTER_MARKET_CONTEXT) ---------------
// kind: bbo | ticker | mark | index | funding | open_interest
inline std::string market_context(std::string_view exchange, std::string_view symbol,
                                  std::string_view kind) {
    return std::string("md.").append(exchange).append(".").append(symbol).append(".").append(kind);
}

// Core NATS live order book (never stored long-term in JetStream).
inline std::string book_snapshot(std::string_view exchange, std::string_view symbol) {
    return std::string("md.").append(exchange).append(".").append(symbol).append(".book.snapshot");
}

inline std::string book_delta(std::string_view exchange, std::string_view symbol) {
    return std::string("md.").append(exchange).append(".").append(symbol).append(".book.delta");
}

// --- Global observation universe (JetStream ALTSPOTTER_MARKET_CONTEXT) --------------
// kind: snapshot | pair.eligible | pair.excluded | pair.changed
inline std::string universe_event(std::string_view kind) {
    return std::string("evt.universe.").append(kind);
}

// --- Storage budget / retention events (JetStream ALTSPOTTER_CONTROL) ---------------
// kind: state | cleanup | critical
inline std::string storage_event(std::string_view kind) {
    return std::string("evt.storage.").append(kind);
}

} // namespace altspotter::subjects
