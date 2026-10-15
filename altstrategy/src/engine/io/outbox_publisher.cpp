#include "altstrategy/engine_io/outbox_publisher.hpp"

#include "altstrategy/engine_io/decision_store.hpp"
#include "altstrategy/logging/logger.hpp"
#include "altstrategy/messaging/nats_client.hpp"

#include <algorithm>
#include <chrono>

namespace altstrategy::engine_io {

namespace {
int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

int64_t outbox_retry_delay_ms(int attempts, const OutboxPublisherConfig& cfg) {
    int64_t d = cfg.retry_base_ms;
    for (int i = 1; i < attempts && d < cfg.retry_cap_ms; ++i) d *= 2;
    return std::min(d, cfg.retry_cap_ms);
}

OutboxPublisher::OutboxPublisher(DecisionStore& store, altstrategy::messaging::NatsClient& nats,
                                 OutboxPublisherConfig cfg)
    : store_(store), nats_(nats), cfg_(cfg) {}

OutboxPublisher::~OutboxPublisher() { stop(); }

void OutboxPublisher::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { loop(); });
}

void OutboxPublisher::stop() {
    if (!running_.exchange(false)) return;
    notify();
    if (thread_.joinable()) thread_.join();
}

void OutboxPublisher::notify() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        notified_ = true;
    }
    cv_.notify_one();
}

int OutboxPublisher::drain_once(int64_t now_ms) {
    const auto due = store_.due_publications(now_ms, cfg_.batch);
    if (!due) return 0;   // the read failure is logged by the store; next pass retries
    int acked = 0;
    for (const auto& p : *due) {
        const auto ack = nats_.js_publish(p.subject, p.payload, p.message_id, cfg_.ack_timeout_ms);
        if (ack) {
            // A row whose mark fails stays due and is sent again: a duplicate
            // Nats-Msg-Id, never a lost message.
            if (store_.mark_published(p.message_id, now_ms, ack->sequence)) ++acked;
            continue;
        }
        failed_.fetch_add(1, std::memory_order_relaxed);
        const int attempts = p.attempts + 1;
        if (now_ms - p.created_at_ms >= cfg_.max_age_ms) {
            dead_lettered_.fetch_add(1, std::memory_order_relaxed);
            store_.mark_dead_lettered(p.message_id, now_ms, ack.error().message);
            log::error("publication_dead_lettered", {{"message_id", p.message_id},
                                                     {"subject", p.subject},
                                                     {"attempts", attempts},
                                                     {"age_ms", now_ms - p.created_at_ms},
                                                     {"error", ack.error().message}});
            continue;
        }
        store_.mark_publish_failed(p.message_id, attempts, now_ms + outbox_retry_delay_ms(attempts, cfg_),
                                   ack.error().message);
        log::warn("publication_failed", {{"message_id", p.message_id},
                                         {"subject", p.subject},
                                         {"attempts", attempts},
                                         {"error", ack.error().message}});
        if (ack.error().retryable) break;
    }
    acked_.fetch_add(acked, std::memory_order_relaxed);
    return acked;
}

void OutboxPublisher::report(int64_t now_ms) {
    if (now_ms - last_report_ms_ < cfg_.report_interval_ms) return;
    last_report_ms_ = now_ms;
    const auto backlog = store_.outbox_backlog();
    if (!backlog) return;
    log::info("publication_outbox",
              {{"acked", acked_.exchange(0)},
               {"failed", failed_.exchange(0)},
               {"dead_lettered_window", dead_lettered_.exchange(0)},
               {"pending", backlog->pending},
               {"oldest_pending_age_ms",
                backlog->oldest_pending_created_at_ms > 0 ? now_ms - backlog->oldest_pending_created_at_ms : 0},
               {"dead_lettered_total", backlog->dead_lettered}});
}

void OutboxPublisher::loop() {
    while (running_.load()) {
        const int64_t now = wall_ms();
        try {
            // A full batch means more is due: go again without waiting.
            if (drain_once(now) >= cfg_.batch) continue;
            report(now);
        } catch (const std::exception& e) {
            log::error("outbox_publisher_exception", {{"message", e.what()}});
        }
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(cfg_.idle_poll_ms),
                     [this] { return notified_ || !running_.load(); });
        notified_ = false;
    }
}

} // namespace altstrategy::engine_io
