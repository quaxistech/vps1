#pragma once

// outbox_publisher — sends to JetStream what the archive committed (BUG-01/02).
//
// A proposal used to be published by a fire-and-forget core-NATS publish right
// after four separate database writes whose results nobody read: it could
// reach AltSignalQuality with no archive behind it, and a publish the bus never
// stored left no trace at all. Now the transaction that archives a decision
// also queues its messages (altstrategy.publication_outbox), and this thread
// sends them with a deterministic Nats-Msg-Id, marking a row only once the
// stream has acknowledged it. A failed row is retried with capped exponential
// backoff until it is acknowledged or, past max_age, dead-lettered and logged
// as an error; a restart resumes from the table.
//
// Delivery is at-least-once. A re-send after an ack this process never
// recorded is a duplicate Nats-Msg-Id -- dropped by the stream inside its
// duplicate window, and de-duplicated by signal_proposal_id downstream after
// it -- never a second business signal.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace altstrategy::messaging { class NatsClient; }

namespace altstrategy::engine_io {

class DecisionStore;

struct OutboxPublisherConfig {
    int64_t ack_timeout_ms = 5'000;
    int64_t retry_base_ms = 1'000;
    int64_t retry_cap_ms = 60'000;
    // A message still unacknowledged this long after its commit is
    // dead-lettered: the signal it carries has long expired downstream.
    int64_t max_age_ms = 24LL * 3600 * 1000;
    int batch = 200;
    int64_t idle_poll_ms = 1'000;
    int64_t report_interval_ms = 60'000;
};

// Delay before the next attempt of a row that has now failed `attempts` times:
// retry_base_ms * 2^(attempts-1), capped at retry_cap_ms.
[[nodiscard]] int64_t outbox_retry_delay_ms(int attempts, const OutboxPublisherConfig& cfg);

class OutboxPublisher {
public:
    OutboxPublisher(DecisionStore& store, altstrategy::messaging::NatsClient& nats,
                    OutboxPublisherConfig cfg = {});
    ~OutboxPublisher();
    OutboxPublisher(const OutboxPublisher&) = delete;
    OutboxPublisher& operator=(const OutboxPublisher&) = delete;

    void start();
    void stop();
    // A commit queued rows: send them now rather than at the next poll.
    void notify();

    // One pass over the rows due at `now_ms`; the number acknowledged. A
    // transport failure ends the pass -- the rest would fail the same way.
    int drain_once(int64_t now_ms);

private:
    void loop();
    void report(int64_t now_ms);

    DecisionStore& store_;
    altstrategy::messaging::NatsClient& nats_;
    OutboxPublisherConfig cfg_;

    std::atomic<bool> running_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    bool notified_ = false;
    std::thread thread_;

    std::atomic<int64_t> acked_{0};
    std::atomic<int64_t> failed_{0};
    std::atomic<int64_t> dead_lettered_{0};
    int64_t last_report_ms_ = 0;
};

} // namespace altstrategy::engine_io
