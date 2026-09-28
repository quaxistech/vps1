// bitget-market-gateway: persistent connection to the Bitget public WebSocket.
//
// Subscribes public trades + 1m candles for all ACTIVE whitelisted instruments
// (loaded from PostgreSQL at startup, updated live via evt.instrument.bitget.>).
//
//   trades              -> Core NATS   md.bitget.<sym>.trade
//   1m candle updates   -> Core NATS   md.bitget.<sym>.candle.1m.update
//   closed 1m candles   -> JetStream   bar.bitget.<sym>.1m.closed  (dedup msg-id)
//   connection events   -> JetStream   evt.gateway.bitget.*
//
// The WS layer (sharding across >=2 connections, ping/pong, reconnect with
// backoff+jitter, resubscribe, stale detection, rotation, outgoing rate
// limits) lives in libs/exchanges/bitget/bitget_ws.

#include "components/components.hpp"
#include "core/readiness.hpp"
#include "core/shutdown.hpp"
#include "config/config.hpp"
#include "exchanges/bitget/bitget_ws.hpp"
#include "logging/logger.hpp"
#include "messaging/codec.hpp"
#include "messaging/nats_client.hpp"
#include "messaging/subjects.hpp"
#include "persistence/pg.hpp"
#include "core/time.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

using namespace altspotter;

namespace {

struct ActiveInstrument {
    int64_t instrument_id = 0;
    std::string symbol;
};

Result<std::map<std::string, int64_t>> load_active_instruments(persistence::PgPool& pool) {
    std::map<std::string, int64_t> out;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec(
            "SELECT ei.exchange_symbol, ei.instrument_id "
            "FROM exchange_instruments ei "
            "JOIN exchanges e ON e.id = ei.exchange_id "
            "WHERE e.code = 'bitget' AND ei.is_whitelisted AND ei.status = 'active' "
            "ORDER BY ei.exchange_symbol");
        for (const auto& r : rows) {
            out.emplace(r[0].as<std::string>(), r[1].as<int64_t>());
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

// JetStream publish deferred to the publisher thread: js_publish waits for the
// server ack (seconds during an outage) and must never run on the WS io thread,
// where it would stall reads/pings and take the exchange connections down with it.
struct PendingPublish {
    std::string subject;
    std::string bytes;
    std::string msg_id;
    std::atomic<int64_t>* published_counter = nullptr;
};

class Gateway {
public:
    Gateway(config::ServiceContext ctx, std::unique_ptr<messaging::NatsClient> nats,
            config::BitgetWsConfig ws_cfg, int connections, bool publish_updates)
        : ctx_(std::move(ctx)), nats_(std::move(nats)),
          stream_(std::make_unique<exchanges::bitget::BitgetWsManager>(std::move(ws_cfg),
                                                                       connections)),
          publish_updates_(publish_updates),
          js_queue_capacity_(static_cast<size_t>(
              config::opt_int(ctx_.service, "js_publish_queue_capacity", 8192))),
          trade_log_(std::chrono::seconds(30)) {}

    ~Gateway() { stop(); }

    void set_instruments(std::map<std::string, int64_t> instruments) {
        std::lock_guard lock(mu_);
        instruments_ = std::move(instruments);
    }

    std::vector<std::string> symbols() const {
        std::lock_guard lock(mu_);
        std::vector<std::string> out;
        out.reserve(instruments_.size());
        for (const auto& [sym, _] : instruments_) out.push_back(sym);
        return out;
    }

    int64_t instrument_id(const std::string& symbol) const {
        std::lock_guard lock(mu_);
        const auto it = instruments_.find(symbol);
        return it == instruments_.end() ? 0 : it->second;
    }

    void start() {
        exchanges::StreamCallbacks cb;
        cb.on_trade = [this](const std::string& sym, const Trade& t, int64_t recv_ms,
                             const std::string& conn_id) {
            on_trade(sym, t, recv_ms, conn_id);
        };
        cb.on_candle_update = [this](const std::string& sym, const Candle& c, int64_t recv_ms,
                                     const std::string& conn_id) {
            on_candle_update(sym, c, recv_ms, conn_id);
        };
        cb.on_candle_closed = [this](const std::string& sym, const Candle& c, int64_t recv_ms,
                                     const std::string& conn_id) {
            on_candle_closed(sym, c, recv_ms, conn_id);
        };
        cb.on_connection_event = [this](const std::string& kind, const std::string& conn_id,
                                        const std::string& detail, int32_t subscribed) {
            on_connection_event(kind, conn_id, detail, subscribed);
        };
        publisher_ = std::thread([this] { publish_loop(); });
        stream_->start(symbols(), std::move(cb));
    }

    void update_subscriptions() { stream_->update_symbols(symbols()); }

    void stop() {
        if (stopped_.exchange(true)) return;
        stream_->stop();
        {
            std::lock_guard lock(q_mu_);
            q_stop_ = true;
        }
        q_cv_.notify_all();
        if (publisher_.joinable()) publisher_.join();
    }

    void log_stats() {
        const int64_t trades = trades_published_.exchange(0);
        const int64_t updates = updates_published_.exchange(0);
        const int64_t closed = closed_published_.exchange(0);
        const int64_t dropped = js_dropped_.exchange(0);
        log::info("gateway_stats", {{"exchange", "bitget"},
                                    {"trades", trades},
                                    {"candle_updates", updates},
                                    {"candles_closed", closed},
                                    {"js_publish_dropped", dropped}});
    }

private:
    void on_trade(const std::string& symbol, const Trade& trade, int64_t recv_ms,
                  const std::string& conn_id) {
        const int64_t iid = instrument_id(symbol);
        if (iid == 0) return;
        messaging::EnvelopeMeta meta;
        meta.event_type = "md.trade";
        meta.instrument_id = iid;
        meta.exchange_symbol = symbol;
        meta.exchange_timestamp_ms = trade.exchange_ts_ms;
        meta.trace_id = conn_id;
        meta.source_service = "market_gateway";
        auto env = messaging::make_envelope(meta);
        env.set_produced_at_ms(recv_ms);
        messaging::to_proto(trade, env.mutable_trade());
        if (auto r = nats_->publish(subjects::trade("bitget", symbol), messaging::serialize(env));
            !r) {
            trade_log_.log(log::Level::warn, "trade_publish", "trade_publish_failed",
                           {{"symbol", symbol}, {"message", r.error().message}});
            return;
        }
        trades_published_.fetch_add(1, std::memory_order_relaxed);
    }

    void on_candle_update(const std::string& symbol, const Candle& candle, int64_t recv_ms,
                          const std::string& conn_id) {
        if (!publish_updates_) return;
        const int64_t iid = instrument_id(symbol);
        if (iid == 0) return;
        messaging::EnvelopeMeta meta;
        meta.event_type = "md.candle.update";
        meta.instrument_id = iid;
        meta.exchange_symbol = symbol;
        meta.exchange_timestamp_ms = candle.open_time_ms;
        meta.trace_id = conn_id;
        meta.source_service = "market_gateway";
        auto env = messaging::make_envelope(meta);
        env.set_produced_at_ms(recv_ms);
        messaging::to_proto(candle, env.mutable_candle());
        if (auto published = nats_->publish(subjects::candle_update("bitget", symbol),
                                            messaging::serialize(env));
            !published) {
            trade_log_.log(log::Level::warn, "candle_update_publish",
                           "candle_update_publish_failed",
                           {{"symbol", symbol}, {"message", published.error().message}});
            return;
        }
        updates_published_.fetch_add(1, std::memory_order_relaxed);
    }

    void on_candle_closed(const std::string& symbol, const Candle& candle, int64_t recv_ms,
                          const std::string& conn_id) {
        const int64_t iid = instrument_id(symbol);
        if (iid == 0) return;
        messaging::EnvelopeMeta meta;
        meta.message_id = messaging::candle_msg_id("bitget", symbol, Timeframe::m1,
                                                   candle.open_time_ms);
        meta.event_type = "bar.closed";
        meta.instrument_id = iid;
        meta.exchange_symbol = symbol;
        meta.exchange_timestamp_ms = recv_ms;
        meta.trace_id = conn_id;
        meta.source_service = "market_gateway";
        auto env = messaging::make_envelope(meta);
        messaging::to_proto(candle, env.mutable_candle());
        enqueue_js({subjects::bar_closed("bitget", symbol, Timeframe::m1),
                    messaging::serialize(env), env.message_id(), &closed_published_});
    }

    void on_connection_event(const std::string& kind, const std::string& conn_id,
                             const std::string& detail, int32_t subscribed) {
        messaging::EnvelopeMeta meta;
        meta.event_type = "gateway." + kind;
        meta.trace_id = conn_id;
        meta.source_service = "market_gateway";
        meta.message_id = "gw:" + kind + ":" + conn_id + ":" + std::to_string(timeutil::now_ms());
        auto env = messaging::make_envelope(meta);
        auto* ev = env.mutable_gateway_event();
        ev->set_type(kind == "connected"      ? altspotter::v1::GATEWAY_EVENT_CONNECTED
                     : kind == "disconnected" ? altspotter::v1::GATEWAY_EVENT_DISCONNECTED
                     : kind == "degraded"     ? altspotter::v1::GATEWAY_EVENT_DEGRADED
                                              : altspotter::v1::GATEWAY_EVENT_RECOVERED);
        ev->set_connection_id(conn_id);
        ev->set_detail(detail);
        ev->set_subscribed_instruments(subscribed);
        enqueue_js({subjects::gateway_event("bitget", kind), messaging::serialize(env),
                    env.message_id(), nullptr});
    }

    // Bounded queue between the WS io thread and the JetStream publisher. On
    // overflow the oldest entry is dropped (REST reconciliation heals candle
    // rows), never blocking the io thread.
    void enqueue_js(PendingPublish p) {
        {
            std::lock_guard lock(q_mu_);
            if (q_stop_) return;
            if (queue_.size() >= js_queue_capacity_) {
                queue_.pop_front();
                js_dropped_.fetch_add(1, std::memory_order_relaxed);
            }
            queue_.push_back(std::move(p));
        }
        q_cv_.notify_one();
    }

    void publish_loop() {
        for (;;) {
            PendingPublish p;
            {
                std::unique_lock lock(q_mu_);
                q_cv_.wait(lock, [&] { return q_stop_ || !queue_.empty(); });
                if (queue_.empty()) return; // q_stop_ and fully drained
                p = std::move(queue_.front());
                queue_.pop_front();
            }
            const bool stopping = [&] {
                std::lock_guard lock(q_mu_);
                return q_stop_;
            }();
            if (auto r = nats_->js_publish(p.subject, p.bytes, p.msg_id); !r) {
                trade_log_.log(log::Level::error, "js_publish", "js_publish_failed",
                               {{"subject", p.subject}, {"message", r.error().message}});
                if (stopping) {
                    // NATS is down during shutdown: do not hang the stop path.
                    std::lock_guard lock(q_mu_);
                    const auto discarded = queue_.size();
                    queue_.clear();
                    if (discarded > 0) {
                        log::warn("js_publish_queue_discarded",
                                  {{"count", discarded},
                                   {"message", "shutdown with JetStream unavailable"}});
                    }
                    return;
                }
            } else if (p.published_counter != nullptr) {
                p.published_counter->fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    config::ServiceContext ctx_;
    std::unique_ptr<messaging::NatsClient> nats_;
    std::unique_ptr<exchanges::bitget::BitgetWsManager> stream_;
    bool publish_updates_;
    mutable std::mutex mu_;
    std::map<std::string, int64_t> instruments_;
    std::atomic<int64_t> trades_published_{0};
    std::atomic<int64_t> updates_published_{0};
    std::atomic<int64_t> closed_published_{0};
    std::atomic<int64_t> js_dropped_{0};
    std::atomic<bool> stopped_{false};
    size_t js_queue_capacity_;
    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::deque<PendingPublish> queue_;
    bool q_stop_ = false;
    std::thread publisher_;
    log::RateLimited trade_log_;
};

} // namespace

namespace altspotter::components {

int market_gateway(config::ServiceContext ctx) {

    auto ws_cfg = config::parse_bitget_ws(ctx.service["bitget"]);
    if (!ws_cfg) {
        log::critical("bad_config", {{"message", ws_cfg.error().message}});
        return 2;
    }
    const int connections = static_cast<int>(config::opt_int(ctx.service, "connections", 2));
    const bool publish_updates = config::opt_bool(ctx.service, "candle_update_publish", true);

    auto nats = messaging::NatsClient::connect(ctx.common.nats_url, ctx.service_name);
    if (!nats) {
        log::critical("nats_unavailable", {{"message", nats.error().message}});
        return 1;
    }

    persistence::PgPool pool(
        ctx.common.postgres_dsn,
        static_cast<size_t>(config::opt_int(ctx.service, "postgres_pool_size", 2)));
    auto instruments = load_active_instruments(pool);
    if (!instruments) {
        log::critical("instrument_load_failed", {{"message", instruments.error().message}});
        return 1;
    }
    if (instruments->empty()) {
        log::warn("no_active_instruments",
                  {{"message", "catalog has no active instruments yet; will follow events"}});
    }

    const std::string nats_url = ctx.common.nats_url;
    const std::string service_name = ctx.service_name;
    Gateway gateway(std::move(ctx), std::move(*nats), std::move(*ws_cfg), connections,
                    publish_updates);
    gateway.set_instruments(std::move(*instruments));

    // Follow catalog updates: any instrument event triggers a reload from PostgreSQL
    // (authoritative state) and a resubscribe if the active set changed.
    auto events_nats = messaging::NatsClient::connect(nats_url, service_name + "-events");
    if (!events_nats) {
        log::critical("nats_unavailable", {{"message", events_nats.error().message}});
        return 1;
    }
    std::atomic<bool> reload_requested{false};
    auto sub = (*events_nats)
                   ->subscribe("evt.instrument.bitget.*", [&](messaging::Msg msg) {
                       (void)msg;
                       reload_requested.store(true, std::memory_order_release);
                   });
    if (!sub) {
        log::warn("instrument_event_subscribe_failed", {{"message", sub.error().message}});
    }

    gateway.start();

    auto& shutdown = ShutdownToken::instance();
    int64_t last_stats = timeutil::now_ms();
    auto known_symbols = gateway.symbols();
    while (!shutdown.wait_for(std::chrono::seconds(1))) {
        if (reload_requested.exchange(false)) {
            auto fresh = load_active_instruments(pool);
            if (fresh) {
                auto fresh_symbols = std::vector<std::string>{};
                fresh_symbols.reserve(fresh->size());
                for (const auto& [s, _] : *fresh) fresh_symbols.push_back(s);
                if (fresh_symbols != known_symbols) {
                    log::info("active_set_changed", {{"count", fresh_symbols.size()}});
                    gateway.set_instruments(std::move(*fresh));
                    gateway.update_subscriptions();
                    known_symbols = std::move(fresh_symbols);
                } else {
                    gateway.set_instruments(std::move(*fresh));
                }
            } else {
                log::warn("instrument_reload_failed", {{"message", fresh.error().message}});
            }
        }
        if (timeutil::now_ms() - last_stats >= 60000) {
            gateway.log_stats();
            last_stats = timeutil::now_ms();
        }
    }

    gateway.stop();
    log::info("service_stopped", {});
    return 0;
}

}  // namespace altspotter::components
