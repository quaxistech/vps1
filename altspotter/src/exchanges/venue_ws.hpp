#pragma once

#include "exchanges/venue_stream.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace altspotter::exchanges {

// One supervised WebSocket session to a venue: connect/TLS/handshake,
// subscribe, heartbeat, stale watchdog, forced rotation, reconnect with
// exponential backoff + jitter, and exception-guarded frame dispatch. The
// engine is venue-agnostic; all protocol specifics live in VenueWsProtocol.
//
// Callbacks run on the session's io thread and must stay non-blocking
// (publish to Core NATS or push into bounded queues only).
class VenueWsSession {
public:
    struct Callbacks {
        std::function<void(VenueEvents&&, int64_t recv_ms, const std::string& conn_id)> on_events;
        // kind: connected | disconnected | degraded
        std::function<void(const std::string& kind, const std::string& conn_id,
                           const std::string& detail)> on_connection_event;
    };

    struct Options {
        int connect_timeout_ms = 8000;
        int stale_timeout_sec = 60;
        int backoff_min_ms = 500;
        int backoff_max_ms = 30000;
    };

    VenueWsSession(std::string conn_id, std::unique_ptr<VenueWsProtocol> protocol,
                   Options options);
    ~VenueWsSession();
    VenueWsSession(const VenueWsSession&) = delete;
    VenueWsSession& operator=(const VenueWsSession&) = delete;

    void start(const std::vector<std::string>& symbols, Callbacks callbacks);
    void update_symbols(const std::vector<std::string>& symbols);
    // Send protocol frames on the live connection (e.g. book resubscribe).
    void send_frames(std::vector<std::string> frames);
    void stop();

    [[nodiscard]] bool connected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace altspotter::exchanges
