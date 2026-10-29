#include "altmacro/core/nats_client.hpp"

#include "altmacro/core/logger.hpp"

#include <chrono>

namespace altmacro::messaging {

namespace {

void on_disconnected(natsConnection* /*nc*/, void* closure) {
    const auto* shutting_down = static_cast<const std::atomic<bool>*>(closure);
    if (shutting_down != nullptr && shutting_down->load(std::memory_order_relaxed)) {
        log::info("nats_disconnected", {{"message", "connection closed for service shutdown"}});
        return;
    }
    log::warn("nats_disconnected", {{"message", "connection to NATS lost, reconnecting"}});
}

void on_reconnected(natsConnection* /*nc*/, void* /*closure*/) {
    log::info("nats_reconnected", {{"message", "connection to NATS restored"}});
}

void on_error(natsConnection* /*nc*/, natsSubscription* /*sub*/, natsStatus err, void* /*closure*/) {
    log::error("nats_async_error", {{"error_code", natsStatus_GetText(err)}});
}

} // namespace

Msg& Msg::operator=(Msg&& o) noexcept {
    if (this != &o) {
        if (msg_ != nullptr) natsMsg_Destroy(msg_);
        msg_ = o.msg_;
        o.msg_ = nullptr;
    }
    return *this;
}

Msg::~Msg() {
    if (msg_ != nullptr) natsMsg_Destroy(msg_);
}

std::string_view Msg::subject() const {
    return msg_ != nullptr ? natsMsg_GetSubject(msg_) : std::string_view{};
}

std::string_view Msg::data() const {
    if (msg_ == nullptr) return {};
    return {natsMsg_GetData(msg_), static_cast<size_t>(natsMsg_GetDataLength(msg_))};
}

uint64_t Msg::delivery_count() const {
    if (msg_ == nullptr) return 0;
    jsMsgMetaData* meta = nullptr;
    if (natsMsg_GetMetaData(&meta, msg_) != NATS_OK || meta == nullptr) return 0;
    const uint64_t delivered = meta->NumDelivered;
    jsMsgMetaData_Destroy(meta);
    return delivered;
}

uint64_t Msg::stream_sequence() const {
    if (msg_ == nullptr) return 0;
    jsMsgMetaData* meta = nullptr;
    if (natsMsg_GetMetaData(&meta, msg_) != NATS_OK || meta == nullptr) return 0;
    const uint64_t seq = meta->Sequence.Stream;
    jsMsgMetaData_Destroy(meta);
    return seq;
}

Result<void> Msg::ack() {
    const natsStatus s = natsMsg_Ack(msg_, nullptr);
    if (s != NATS_OK) return std::unexpected(nats_error(s, "ack"));
    return {};
}

Result<void> Msg::nak(int64_t delay_ms) {
    natsStatus s;
    if (delay_ms > 0) {
        s = natsMsg_NakWithDelay(msg_, delay_ms, nullptr);
    } else {
        s = natsMsg_Nak(msg_, nullptr);
    }
    if (s != NATS_OK) return std::unexpected(nats_error(s, "nak"));
    return {};
}

Result<void> Msg::term() {
    const natsStatus s = natsMsg_Term(msg_, nullptr);
    if (s != NATS_OK) return std::unexpected(nats_error(s, "term"));
    return {};
}

PullConsumer::~PullConsumer() {
    if (sub_ != nullptr) natsSubscription_Destroy(sub_);
}

Result<std::vector<Msg>> PullConsumer::fetch(int batch, int64_t timeout_ms) {
    natsMsgList list = {nullptr, 0};
    const natsStatus s = natsSubscription_Fetch(&list, sub_, batch, timeout_ms, nullptr);
    if (s == NATS_TIMEOUT) return std::vector<Msg>{};
    if (s != NATS_OK) return std::unexpected(nats_error(s, "fetch"));

    std::vector<Msg> out;
    out.reserve(static_cast<size_t>(list.Count));
    for (int i = 0; i < list.Count; ++i) {
        out.emplace_back(list.Msgs[i]);
        list.Msgs[i] = nullptr; // ownership moved into Msg
    }
    // Only the array is freed here; messages were detached above.
    natsMsgList_Destroy(&list);
    return out;
}

Subscription::~Subscription() {
    if (sub_ != nullptr) natsSubscription_Destroy(sub_);
}

Result<std::unique_ptr<NatsClient>> NatsClient::connect(const std::string& url,
                                                        const std::string& client_name) {
    auto client = std::unique_ptr<NatsClient>(new NatsClient());

    natsStatus s = natsOptions_Create(&client->opts_);
    if (s == NATS_OK) s = natsOptions_SetURL(client->opts_, url.c_str());
    if (s == NATS_OK) s = natsOptions_SetName(client->opts_, client_name.c_str());
    if (s == NATS_OK) s = natsOptions_SetTimeout(client->opts_, 5000);
    if (s == NATS_OK) s = natsOptions_SetMaxReconnect(client->opts_, -1); // unlimited
    if (s == NATS_OK) s = natsOptions_SetReconnectWait(client->opts_, 1000);
    if (s == NATS_OK) s = natsOptions_SetReconnectJitter(client->opts_, 500, 2000);
    if (s == NATS_OK) s = natsOptions_SetReconnectBufSize(client->opts_, 16 * 1024 * 1024);
    if (s == NATS_OK) {
        s = natsOptions_SetDisconnectedCB(client->opts_, on_disconnected,
                                          &client->shutting_down_);
    }
    if (s == NATS_OK) s = natsOptions_SetReconnectedCB(client->opts_, on_reconnected, nullptr);
    if (s == NATS_OK) s = natsOptions_SetClosedCB(client->opts_, &NatsClient::on_closed, client.get());
    if (s == NATS_OK) s = natsOptions_SetErrorHandler(client->opts_, on_error, nullptr);
    if (s == NATS_OK) s = natsConnection_Connect(&client->conn_, client->opts_);
    if (s != NATS_OK) return std::unexpected(nats_error(s, "connect to NATS"));

    jsOptions jso;
    jsOptions_Init(&jso);
    jso.PublishAsync.MaxPending = 4096;
    s = natsConnection_JetStream(&client->js_, client->conn_, &jso);
    if (s != NATS_OK) return std::unexpected(nats_error(s, "JetStream context"));

    return client;
}

void NatsClient::on_closed(natsConnection* /*nc*/, void* closure) {
    auto* self = static_cast<NatsClient*>(closure);
    // Notified under the lock: a destructor that wakes on its own and sees
    // closed_ cannot release closed_cv_ before this call has finished with it.
    std::lock_guard<std::mutex> lk(self->closed_mu_);
    self->closed_ = true;
    self->closed_cv_.notify_all();
}

NatsClient::~NatsClient() {
    shutting_down_.store(true, std::memory_order_relaxed);
    if (js_ != nullptr) jsCtx_Destroy(js_);
    if (conn_ != nullptr) {
        natsConnection_Close(conn_);
        {
            // Bounded: a library that never calls back must not hang a stop.
            std::unique_lock<std::mutex> lk(closed_mu_);
            closed_cv_.wait_for(lk, std::chrono::seconds(5), [this] { return closed_; });
        }
        natsConnection_Destroy(conn_);
    }
    if (opts_ != nullptr) natsOptions_Destroy(opts_);
}

Result<void> NatsClient::publish(const std::string& subject, std::string_view bytes) {
    const natsStatus s = natsConnection_Publish(conn_, subject.c_str(), bytes.data(),
                                                static_cast<int>(bytes.size()));
    if (s != NATS_OK) return std::unexpected(nats_error(s, "publish " + subject));
    return {};
}

Result<void> NatsClient::js_publish(const std::string& subject, std::string_view bytes,
                                    const std::string& msg_id, int64_t timeout_ms) {
    jsPubOptions po;
    jsPubOptions_Init(&po);
    po.MaxWait = timeout_ms;
    if (!msg_id.empty()) po.MsgId = msg_id.c_str();

    jsPubAck* ack = nullptr;
    jsErrCode jerr = static_cast<jsErrCode>(0);
    const natsStatus s = js_Publish(&ack, js_, subject.c_str(), bytes.data(),
                                    static_cast<int>(bytes.size()), &po, &jerr);
    if (ack != nullptr) jsPubAck_Destroy(ack);
    if (s != NATS_OK) {
        return std::unexpected(nats_error(
            s, "js_publish " + subject + " (jetstream err " + std::to_string(jerr) + ")"));
    }
    return {};
}

namespace {
struct HandlerClosure {
    NatsClient::MsgHandler* fn;
};

void dispatch_msg(natsConnection* /*nc*/, natsSubscription* /*sub*/, natsMsg* msg, void* closure) {
    auto* h = static_cast<NatsClient::MsgHandler*>(closure);
    (*h)(Msg{msg});
}
} // namespace

Result<Subscription> NatsClient::subscribe(const std::string& subject, MsgHandler handler) {
    auto stored = std::make_unique<MsgHandler>(std::move(handler));
    natsSubscription* sub = nullptr;
    const natsStatus s =
        natsConnection_Subscribe(&sub, conn_, subject.c_str(), dispatch_msg, stored.get());
    if (s != NATS_OK) return std::unexpected(nats_error(s, "subscribe " + subject));
    handlers_.push_back(std::move(stored));
    return Subscription{sub};
}

Result<PullConsumer> NatsClient::pull_consumer(const std::string& stream,
                                               const std::string& durable) {
    natsSubscription* sub = nullptr;
    jsErrCode jerr = static_cast<jsErrCode>(0);
    jsSubOptions so;
    jsSubOptions_Init(&so);
    // Bind to the pre-created durable consumer (altmacro nats-setup owns creation).
    so.Stream = stream.c_str();
    so.Consumer = durable.c_str();
    const natsStatus s = js_PullSubscribe(&sub, js_, nullptr, nullptr, nullptr, &so, &jerr);
    if (s != NATS_OK) {
        return std::unexpected(nats_error(s, "pull_subscribe " + stream + "/" + durable +
                                                 " (jetstream err " + std::to_string(jerr) + ")"));
    }
    return PullConsumer{sub};
}

bool NatsClient::connected() const {
    return conn_ != nullptr &&
           natsConnection_Status(conn_) == NATS_CONN_STATUS_CONNECTED;
}

void NatsClient::drain(int64_t timeout_ms) {
    if (conn_ != nullptr) {
        natsConnection_FlushTimeout(conn_, timeout_ms);
    }
}

} // namespace altmacro::messaging
