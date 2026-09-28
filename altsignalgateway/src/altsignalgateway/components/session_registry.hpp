#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace altsignalgateway::components {

class Session;

// The set of live WebSocket sessions.
//
// DUPLICATE SESSION POLICY: newest wins.
//
// When an authenticated client_id opens a second session beyond its configured
// max_sessions, the OLDEST session for that client is closed with
// ERR_SESSION_SUPERSEDED and the new one proceeds. The alternative — refusing
// the new connection — was rejected deliberately: after a network partition the
// server frequently still holds a half-dead socket that will not fail until its
// heartbeat budget runs out, and locking a reconnecting consumer out of its own
// feed for that long is the worse failure. Nothing is lost either way: the
// superseded session requeues everything it held, and the new one collects it
// through RESUME.
//
// The policy is applied under this class's lock, so two simultaneous
// connections cannot both decide they are the newest.
class SessionRegistry {
public:
    explicit SessionRegistry(int32_t max_clients, int32_t auth_failure_threshold,
                             int32_t auth_failure_window_sec, int32_t auth_lockout_sec);

    // Called once the WebSocket handshake completes, BEFORE authentication:
    // an unauthenticated connection already consumes a slot, and letting an
    // unbounded number of them queue up for authentication is the cheapest
    // denial of service there is.
    bool attach(const std::shared_ptr<Session>& session);
    void detach(const std::string& session_id);

    // Applied after authentication, when the client identity is known.
    // Returns the sessions that must be superseded.
    std::vector<std::shared_ptr<Session>> enforce_client_limit(const std::string& client_id,
                                                               const std::string& keep_session_id,
                                                               int32_t max_sessions);

    [[nodiscard]] std::vector<std::shared_ptr<Session>> snapshot() const;
    [[nodiscard]] size_t size() const;
    [[nodiscard]] size_t authenticated_count() const;

    // --- authentication back-off --------------------------------------------
    // A per-peer failure counter with a lock-out window. It bounds an online
    // guessing attack against the HMAC keys without ever locking out a
    // legitimate client that simply restarted: a successful authentication
    // clears the counter.
    [[nodiscard]] bool is_locked_out(const std::string& peer) const;
    void note_auth_failure(const std::string& peer);
    void note_auth_success(const std::string& peer);

private:
    struct Entry {
        std::shared_ptr<Session> session;
        // Monotone attach order. Session ids are random by design (they are a
        // nonce component of the handshake), so "oldest" cannot be derived from
        // them; this counter is the only thing that knows.
        uint64_t ordinal = 0;
    };

    struct FailureRecord {
        int32_t count = 0;
        std::chrono::steady_clock::time_point window_start{};
        std::chrono::steady_clock::time_point locked_until{};
    };

    mutable std::mutex mu_;
    std::map<std::string, Entry> sessions_;                      // session_id -> entry
    uint64_t next_ordinal_ = 1;
    std::map<std::string, FailureRecord> failures_;              // peer -> record
    int32_t max_clients_;
    int32_t auth_failure_threshold_;
    int32_t auth_failure_window_sec_;
    int32_t auth_lockout_sec_;
};

} // namespace altsignalgateway::components
