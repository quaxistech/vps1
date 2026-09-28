#pragma once

#include "altsignalgateway/core/config.hpp"
#include "altsignalgateway/core/nats_client.hpp"
#include "altsignalgateway/data/store.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace altsignalgateway::components {

// The ingest path: AltSignalQuality's FINAL TradeSignal -> the durable journal.
//
//   consume -> validate -> enrich -> canonicalize -> hash -> persist -> ACK
//
// The ACK to JetStream is the LAST step, and it is only ever sent after the
// journal transaction has committed. That ordering is the whole reason this
// component exists as its own loop: acknowledging first and writing afterwards
// would mean a crash in the gap loses a trade with no trace and no way to ask
// for it again, because the durable's cursor has already moved past it.
//
// The consequence is accepted deliberately: a crash AFTER the commit and BEFORE
// the ack redelivers the message, and the deterministic signal id makes the
// second attempt a no-op. At-least-once in, exactly-once in the journal.
class Ingestor {
public:
    Ingestor(const config::RuntimeConfig& cfg, std::shared_ptr<data::JournalStore> store,
             std::string gateway_version);
    ~Ingestor();

    void start();
    void stop();

    // Called after each journalled signal so the delivery pump can pick it up
    // without waiting for its next tick.
    void on_journalled(std::function<void()> cb) { on_journalled_ = std::move(cb); }

    [[nodiscard]] bool connected() const { return connected_.load(std::memory_order_acquire); }
    [[nodiscard]] int64_t journalled() const { return journalled_.load(std::memory_order_acquire); }
    [[nodiscard]] int64_t duplicates() const { return duplicates_.load(std::memory_order_acquire); }
    [[nodiscard]] int64_t rejected() const { return rejected_.load(std::memory_order_acquire); }
    [[nodiscard]] int64_t conflicts() const { return conflicts_.load(std::memory_order_acquire); }
    // When the consume loop last completed an iteration. Process liveness,
    // reported separately from `connected()`, which is dependency health.
    [[nodiscard]] int64_t last_tick_ms() const {
        return last_tick_ms_.load(std::memory_order_acquire);
    }
    [[nodiscard]] int64_t last_signal_at_ms() const {
        return last_signal_at_ms_.load(std::memory_order_acquire);
    }

private:
    void run();
    // Handle one inbound message. Returns true when the message may be acked,
    // false when it must be nak'd for redelivery. A message that is refused on
    // contract grounds is terminated inside and reported as ackable, because
    // redelivering it forever would wedge the consumer on a message that will
    // never become valid.
    bool handle(messaging::Msg& msg);

    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<int64_t> journalled_{0};
    std::atomic<int64_t> duplicates_{0};
    std::atomic<int64_t> rejected_{0};
    std::atomic<int64_t> conflicts_{0};
    std::atomic<int64_t> last_signal_at_ms_{0};
    std::atomic<int64_t> last_tick_ms_{0};
    std::function<void()> on_journalled_;
};

} // namespace altsignalgateway::components
