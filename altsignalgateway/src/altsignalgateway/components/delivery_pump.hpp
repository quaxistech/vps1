#pragma once

#include "altsignalgateway/components/session.hpp"
#include "altsignalgateway/components/session_registry.hpp"
#include "altsignalgateway/core/config.hpp"
#include "altsignalgateway/data/store.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace altsignalgateway::components {

// The delivery pump: the ONLY thread that writes the delivery side of the
// journal, and the only place a signal is turned into bytes on the wire.
//
// WHY A SINGLE THREAD. Two properties fall out of it that would otherwise need
// locks spanning a socket:
//
//   * ORDERING. Every durable transition a session causes (SENT, then ACKED)
//     is queued in submission order and applied in that order, so the delivery
//     state machine can never see an ACK for a message it has not recorded as
//     sent.
//   * ISOLATION. No PostgreSQL call ever runs on an I/O thread. A slow database
//     slows delivery; it does not stall the event loop, the heartbeats or the
//     handshake of every other connection.
//
// WHY THE JOURNAL IS THE QUEUE. The pump reads the backlog from PostgreSQL in
// bounded windows sized by what the client can actually accept. The process
// therefore holds at most `max_queued_messages` signals per session in RAM
// regardless of how far behind the consumer is — a week-long backlog and an
// empty one cost the same memory.
class DeliveryPump {
public:
    DeliveryPump(const config::RuntimeConfig& cfg, std::shared_ptr<data::JournalStore> store,
                 SessionRegistry& registry, std::string gateway_version);
    ~DeliveryPump();

    void start();
    void stop();

    // Thread-safe. Queues a durable write; never blocks the caller.
    void submit(JournalOp op);
    // Thread-safe. Asks the pump to run a pass now instead of at the next tick.
    void wake();

    // Queue a read of a client's durable cursor and the journal head. The reply
    // runs on the pump thread; callers post it back to their own executor.
    void query_cursor(std::string client_id, std::function<void(int64_t, int64_t)> reply);
    // Queue a read of what is still owed above `from`: (deliverable, expired).
    void query_backlog(int64_t from, std::function<void(int64_t, int64_t)> reply);

    [[nodiscard]] int64_t journal_head() const { return head_.load(std::memory_order_acquire); }
    [[nodiscard]] int64_t last_error_at_ms() const {
        return last_error_at_ms_.load(std::memory_order_acquire);
    }
    // Whether the last database operation succeeded. This is DEPENDENCY health,
    // not process health: it goes false whenever PostgreSQL is unreachable, and
    // a restart would not fix that.
    [[nodiscard]] bool healthy() const { return healthy_.load(std::memory_order_acquire); }
    // When this thread last completed a loop iteration. This is PROCESS
    // liveness: a value that stops advancing means the pump is wedged, which is
    // the one condition a restart actually repairs.
    [[nodiscard]] int64_t last_tick_ms() const {
        return last_tick_ms_.load(std::memory_order_acquire);
    }

private:
    void run();
    void drain_ops();
    void apply(JournalOp& op);
    void pump_sessions();
    void maintenance();
    // The journal's real head, read fresh. Transmitted to clients, so it is not
    // served from the cached value.
    int64_t current_head();
    // Build one transmissible frame from a journal row, verifying on the way
    // that the stored payload still hashes to what the journal claims.
    bool prepare(const data::JournalRecord& rec, bool replay, OutboundSignal* out);

    config::RuntimeConfig cfg_;
    std::shared_ptr<data::JournalStore> store_;
    SessionRegistry& registry_;
    std::string gateway_version_;
    // ingest.max_signal_age_sec in milliseconds; 0 disables the age rule and
    // leaves each signal's own validity window as the only freshness bound.
    int64_t max_signal_age_ms_ = 0;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<JournalOp> ops_;

    std::atomic<int64_t> head_{0};
    std::atomic<int64_t> last_error_at_ms_{0};
    std::atomic<bool> healthy_{true};
    std::atomic<int64_t> last_tick_ms_{0};
    int64_t last_sweep_ms_ = 0;
    int64_t last_retention_ms_ = 0;
    // Per-session replay accounting, owned exclusively by the pump thread.
    std::map<std::string, int64_t> replayed_;
};

} // namespace altsignalgateway::components
