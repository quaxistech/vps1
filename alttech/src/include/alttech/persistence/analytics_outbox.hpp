#pragma once

#include "alttech/persistence/pg.hpp"
#include "alttech/messaging/nats_client.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace alttech::persistence {

inline void enqueue_analytics(pqxx::work& txn, const std::string& id,
                              const std::string& subject, std::string_view bytes) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(bytes.size() * 2);
    for (const unsigned char c : bytes) { encoded += hex[c >> 4U]; encoded += hex[c & 15U]; }
    txn.exec_params("INSERT INTO alttech.analytics_outbox(message_id,subject,payload) "
                    "VALUES($1,$2,decode($3,'hex')) ON CONFLICT(message_id) DO NOTHING", id, subject, encoded);
}

// Commit the notification with the owner row, then deliver from this bounded
// worker. Crash after broker ACK replays the same id; no vector recomputation.
inline Result<void> drain_analytics_outbox(PgPool& pool, messaging::NatsClient& nats) {
    return pool.with_txn([&](pqxx::work& txn) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        const auto rows = txn.exec("SELECT message_id,subject,payload FROM alttech.analytics_outbox "
                                   "ORDER BY created_at,message_id FOR UPDATE SKIP LOCKED LIMIT 256");
        for (const auto& row : rows) {
            if (std::chrono::steady_clock::now() >= deadline) break;
            const std::string id = row[0].as<std::string>();
            const auto payload = row[2].as<std::basic_string<std::byte>>();
            const auto sent = nats.js_publish(row[1].as<std::string>(),
                std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()), id, 100);
            if (!sent) break;
            txn.exec_params("DELETE FROM alttech.analytics_outbox WHERE message_id=$1", id);
        }
    });
}

} // namespace alttech::persistence
