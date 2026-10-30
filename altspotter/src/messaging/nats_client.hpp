#pragma once

#include "core/error.hpp"

#include <nats.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace altspotter::messaging {

// Owned inbound message (JetStream or core). Ack/Nak only meaningful for JetStream.
class Msg {
public:
    explicit Msg(natsMsg* m) : msg_(m) {}
    Msg(const Msg&) = delete;
    Msg& operator=(const Msg&) = delete;
    Msg(Msg&& o) noexcept : msg_(o.msg_) { o.msg_ = nullptr; }
    Msg& operator=(Msg&& o) noexcept;
    ~Msg();

    [[nodiscard]] std::string_view subject() const;
    [[nodiscard]] std::string_view data() const;
    Result<void> ack();
    Result<void> nak(int64_t delay_ms = 0);
    Result<void> term();

private:
    natsMsg* msg_ = nullptr;
};

class PullConsumer {
public:
    PullConsumer(natsSubscription* sub) : sub_(sub) {}
    PullConsumer(const PullConsumer&) = delete;
    PullConsumer& operator=(const PullConsumer&) = delete;
    PullConsumer(PullConsumer&& o) noexcept : sub_(o.sub_) { o.sub_ = nullptr; }
    ~PullConsumer();

    // Fetch up to `batch` messages, waiting at most timeout_ms. Empty vector on timeout.
    Result<std::vector<Msg>> fetch(int batch, int64_t timeout_ms);

private:
    natsSubscription* sub_ = nullptr;
};

class Subscription {
public:
    explicit Subscription(natsSubscription* sub) : sub_(sub) {}
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&& o) noexcept : sub_(o.sub_) { o.sub_ = nullptr; }
    ~Subscription();

private:
    natsSubscription* sub_ = nullptr;
};

// Thread-safe NATS client (nats.c is internally thread-safe). Reconnects
// automatically with unlimited attempts and buffered publishes.
class NatsClient {
public:
    using MsgHandler = std::function<void(Msg)>;

    static Result<std::unique_ptr<NatsClient>> connect(const std::string& url,
                                                       const std::string& client_name);

    ~NatsClient();
    NatsClient(const NatsClient&) = delete;
    NatsClient& operator=(const NatsClient&) = delete;

    // Core publish (fire-and-forget, buffered).
    Result<void> publish(const std::string& subject, std::string_view bytes);

    // JetStream publish with deduplication id; synchronous ack from the server.
    Result<void> js_publish(const std::string& subject, std::string_view bytes,
                            const std::string& msg_id, int64_t timeout_ms = 10000);

    // Async core subscription; handler runs on the NATS delivery thread.
    Result<Subscription> subscribe(const std::string& subject, MsgHandler handler);

    // Bind to a pre-created durable pull consumer (created by altspotter nats-init).
    Result<PullConsumer> pull_consumer(const std::string& stream, const std::string& durable);

    [[nodiscard]] bool connected() const;
    [[nodiscard]] natsConnection* raw() const { return conn_; }
    [[nodiscard]] jsCtx* js() const { return js_; }

    // Blocks up to timeout draining buffered messages before destruction.
    void drain(int64_t timeout_ms);

private:
    NatsClient() = default;
    // The library's closed callback: the last one it queues for a connection.
    static void on_closed(natsConnection* nc, void* closure);

    natsConnection* conn_ = nullptr;
    natsOptions* opts_ = nullptr;
    jsCtx* js_ = nullptr;
    std::atomic<bool> shutting_down_{false};
    // The library calls back on its own thread, and the disconnected callback
    // reads shutting_down_ and logs: freed while that callback was still queued,
    // it was read after release, and the logger it wrote to could already be
    // destroyed by exit (a SIGSEGV of a process stopping right after its
    // connection closed). The destructor waits for the closed callback, queued
    // after every other one.
    std::mutex closed_mu_;
    std::condition_variable closed_cv_;
    bool closed_ = false;
    std::vector<std::unique_ptr<MsgHandler>> handlers_;
};

[[nodiscard]] inline Error nats_error(natsStatus s, const std::string& context) {
    return Error{ErrorCode::messaging_error, context + ": " + natsStatus_GetText(s),
                 s == NATS_TIMEOUT || s == NATS_CONNECTION_CLOSED || s == NATS_NO_RESPONDERS};
}

} // namespace altspotter::messaging
