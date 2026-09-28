#pragma once

#include "altmacro/core/error.hpp"

#include <nats.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace altmacro::messaging {

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
    // How many times JetStream has delivered this message, 1 on first delivery.
    // 0 when the message carries no JetStream metadata (a core subscription).
    [[nodiscard]] uint64_t delivery_count() const;
    // The message's position in the producer's stream. 0 when the message
    // carries no JetStream metadata. Recorded in the journal so a signal can be
    // traced back to the exact stream offset it arrived on.
    [[nodiscard]] uint64_t stream_sequence() const;
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

    // Bind to a pre-created durable pull consumer (created by altmacro nats-setup).
    Result<PullConsumer> pull_consumer(const std::string& stream, const std::string& durable);

    [[nodiscard]] bool connected() const;
    [[nodiscard]] natsConnection* raw() const { return conn_; }
    [[nodiscard]] jsCtx* js() const { return js_; }

    // Blocks up to timeout draining buffered messages before destruction.
    void drain(int64_t timeout_ms);

private:
    NatsClient() = default;

    natsConnection* conn_ = nullptr;
    natsOptions* opts_ = nullptr;
    jsCtx* js_ = nullptr;
    std::atomic<bool> shutting_down_{false};
    std::vector<std::unique_ptr<MsgHandler>> handlers_;
};

[[nodiscard]] inline Error nats_error(natsStatus s, const std::string& context) {
    return Error{ErrorCode::messaging_error, context + ": " + natsStatus_GetText(s),
                 s == NATS_TIMEOUT || s == NATS_CONNECTION_CLOSED || s == NATS_NO_RESPONDERS};
}

} // namespace altmacro::messaging
