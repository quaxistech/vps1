#pragma once

#include "logging/logger.hpp"
#include "messaging/nats_client.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace altspotter::messaging {

// Bounded queue + dedicated publisher thread for JetStream publishes.
// js_publish waits for the server ack (seconds during an outage) and must
// never run on a WS io thread, where it would stall reads/pings and take the
// exchange connections down with it. On overflow the oldest entry is dropped
// (REST reconciliation / periodic snapshots heal), never blocking the caller.
class JsPublishQueue {
public:
    struct Item {
        std::string subject;
        std::string bytes;
        std::string msg_id;
        std::atomic<int64_t>* published_counter = nullptr; // optional stats hook
    };

    JsPublishQueue(NatsClient& nats, size_t capacity, std::string log_key)
        : nats_(nats), capacity_(capacity == 0 ? 1 : capacity),
          log_key_(std::move(log_key)), err_log_(std::chrono::seconds(30)) {
        worker_ = std::thread([this] { run(); });
    }

    ~JsPublishQueue() { stop(); }
    JsPublishQueue(const JsPublishQueue&) = delete;
    JsPublishQueue& operator=(const JsPublishQueue&) = delete;

    void enqueue(Item item) {
        int64_t drop = 0;
        {
            std::lock_guard lock(mu_);
            if (stop_) return;
            if (queue_.size() >= capacity_) {
                queue_.pop_front();
                // Report it. A dropped publish is market data that never
                // reaches the bus, and until now this counter was incremented
                // and never read by anyone: `exchange_runtime` — the publish
                // path for three of the four venues — creates this queue and
                // has no periodic stats line, so overflow was invisible.
                // Throttled, because an overflow that happens once happens
                // thousands of times.
                dropped_total_.fetch_add(1, std::memory_order_relaxed);
                drop = dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
            }
            queue_.push_back(std::move(item));
        }
        if (drop > 0) {
            err_log_.log(log::Level::warn, log_key_ + ":drop", "js_publish_dropped",
                         {{"context", log_key_},
                          {"dropped_total", dropped_total_.load(std::memory_order_relaxed)},
                          {"capacity", capacity_},
                          {"reason", "publish queue full; oldest entry discarded"}});
        }
        cv_.notify_one();
    }

    // Drains remaining items best-effort; if NATS is down, discards the rest
    // so shutdown never hangs on the ack timeout chain.
    void stop() {
        {
            std::lock_guard lock(mu_);
            if (stop_) return;
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    [[nodiscard]] int64_t dropped_and_reset() {
        return dropped_.exchange(0, std::memory_order_relaxed);
    }

private:
    void run() {
        for (;;) {
            Item item;
            bool stopping = false;
            {
                std::unique_lock lock(mu_);
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) return; // stop_ and fully drained
                item = std::move(queue_.front());
                queue_.pop_front();
                stopping = stop_;
            }
            if (auto r = nats_.js_publish(item.subject, item.bytes, item.msg_id); !r) {
                err_log_.log(log::Level::error, log_key_, "js_publish_failed",
                             {{"subject", item.subject}, {"message", r.error().message}});
                if (stopping) {
                    std::lock_guard lock(mu_);
                    if (!queue_.empty()) {
                        log::warn("js_publish_queue_discarded",
                                  {{"count", queue_.size()}, {"context", log_key_}});
                        queue_.clear();
                    }
                    return;
                }
            } else if (item.published_counter != nullptr) {
                item.published_counter->fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    NatsClient& nats_;
    size_t capacity_;
    std::string log_key_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    bool stop_ = false;
    std::thread worker_;
    std::atomic<int64_t> dropped_{0};        // consumed by dropped_and_reset()
    std::atomic<int64_t> dropped_total_{0};  // lifetime count, for the log line
    log::RateLimited err_log_;
};

} // namespace altspotter::messaging
