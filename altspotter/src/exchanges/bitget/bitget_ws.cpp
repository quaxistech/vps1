#include "exchanges/bitget/bitget_ws.hpp"

#include "core/backoff.hpp"
#include "logging/logger.hpp"
#include "core/time.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace altspotter::exchanges::bitget {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

namespace {

struct UrlParts {
    std::string host;
    std::string port = "443";
    std::string path = "/";
};

UrlParts parse_wss_url(const std::string& url) {
    UrlParts p;
    std::string rest = url;
    if (const auto pos = rest.find("://"); pos != std::string::npos) rest = rest.substr(pos + 3);
    const auto slash = rest.find('/');
    p.host = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (slash != std::string::npos) p.path = rest.substr(slash);
    if (const auto colon = p.host.find(':'); colon != std::string::npos) {
        p.port = p.host.substr(colon + 1);
        p.host = p.host.substr(0, colon);
    }
    return p;
}

Result<Candle> parse_ws_candle_row(const json& row) {
    // ["1695685500000","o","h","l","c","baseVol","quoteVol","usdtVol"]
    if (!row.is_array() || row.size() < 7) {
        return fail(ErrorCode::parse_error, "ws candle row has unexpected shape");
    }
    Candle c;
    c.timeframe = Timeframe::m1;
    const auto dec = [&](size_t i) -> Result<Decimal> {
        if (!row[i].is_string()) return fail(ErrorCode::parse_error, "non-string decimal in ws candle");
        return Decimal::parse(row[i].get_ref<const std::string&>());
    };
    int64_t ts = 0;
    if (row[0].is_string()) {
        try {
            ts = std::stoll(row[0].get_ref<const std::string&>());
        } catch (const std::exception&) {
            return fail(ErrorCode::parse_error, "invalid ws candle timestamp");
        }
    } else {
        return fail(ErrorCode::parse_error, "invalid ws candle timestamp type");
    }
    c.open_time_ms = ts;
    c.close_time_ms = ts + timeutil::kMsPerMinute;
    auto o = dec(1), h = dec(2), l = dec(3), cl = dec(4), bv = dec(5), qv = dec(6);
    for (const auto* r : {&o, &h, &l, &cl, &bv, &qv}) {
        if (!*r) return std::unexpected(r->error());
    }
    c.open = *o;
    c.high = *h;
    c.low = *l;
    c.close = *cl;
    c.base_volume = *bv;
    c.quote_volume = *qv;
    c.is_closed = false; // closes when the next open_time appears
    c.source = CandleSource::ws;
    c.quality = Quality::ok;
    return c;
}

} // namespace

struct BitgetWsManager::Impl {
    using WsStream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    struct SymbolState {
        Candle last_candle;          // latest update of the current (open) minute
        bool has_candle = false;
        int64_t last_closed_open_ms = -1; // dedup: never re-publish closed <= this
    };

    struct Connection {
        int index = 0;
        std::string id;                      // "ws-0", rotated ids get a suffix
        std::unique_ptr<WsStream> ws;
        std::vector<std::string> symbols;    // assigned shard
        std::deque<std::string> outbox;
        bool writing = false;
        bool open = false;
        int64_t last_inbound_mono_ms = 0;    // steady clock ms of last frame
        int64_t last_ping_sent_mono_ms = 0;
        bool awaiting_pong = false;
        Backoff backoff;
        int generation = 0;
        asio::steady_timer ping_timer;
        asio::steady_timer watchdog_timer;
        asio::steady_timer rotation_timer;
        std::map<std::string, SymbolState> state;

        Connection(asio::io_context& ioc, int idx, const config::BitgetWsConfig& cfg)
            : index(idx), id("ws-" + std::to_string(idx)),
              backoff(std::chrono::milliseconds(cfg.backoff_min_ms),
                      std::chrono::milliseconds(cfg.backoff_max_ms)),
              ping_timer(ioc), watchdog_timer(ioc), rotation_timer(ioc) {}
    };

    config::BitgetWsConfig cfg;
    int n_connections;
    UrlParts url;
    asio::io_context ioc;
    asio::ssl::context ssl_ctx{asio::ssl::context::tls_client};
    std::vector<std::unique_ptr<Connection>> conns;
    StreamCallbacks callbacks;
    std::vector<std::string> all_symbols;
    std::atomic<bool> stopping{false};
    std::thread io_thread;
    double outgoing_tokens = 0;
    int64_t outgoing_refill_mono_ms = 0;

    Impl(config::BitgetWsConfig c, int n) : cfg(std::move(c)), n_connections(std::max(2, n)) {
        url = parse_wss_url(cfg.ws_public_url);
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(asio::ssl::verify_peer);
    }

    static int64_t mono_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // ---- sharding -----------------------------------------------------------
    void reshard() {
        for (auto& c : conns) c->symbols.clear();
        // Round-robin keeps shards balanced; both channels of a symbol share a shard.
        const size_t per_conn_cap = static_cast<size_t>(cfg.max_channels_per_connection) / 2;
        size_t idx = 0;
        for (const auto& sym : all_symbols) {
            for (size_t attempt = 0; attempt < conns.size(); ++attempt) {
                auto& c = conns[idx % conns.size()];
                ++idx;
                if (c->symbols.size() < per_conn_cap) {
                    c->symbols.push_back(sym);
                    break;
                }
            }
        }
        size_t assigned = 0;
        for (auto& c : conns) assigned += c->symbols.size();
        if (assigned < all_symbols.size()) {
            log::error("ws_channel_capacity_exceeded",
                       {{"message", "not all symbols fit into channel capacity"},
                        {"assigned", assigned},
                        {"total", all_symbols.size()}});
        }
    }

    // ---- outgoing queue -----------------------------------------------------
    bool take_outgoing_token() {
        const int64_t now = mono_ms();
        if (outgoing_refill_mono_ms == 0) outgoing_refill_mono_ms = now;
        const double rate = static_cast<double>(cfg.max_outgoing_messages_per_second);
        outgoing_tokens = std::min(rate, outgoing_tokens + rate *
                                             static_cast<double>(now - outgoing_refill_mono_ms) /
                                             1000.0);
        outgoing_refill_mono_ms = now;
        if (outgoing_tokens >= 1.0) {
            outgoing_tokens -= 1.0;
            return true;
        }
        return false;
    }

    void enqueue(Connection& c, std::string msg) {
        c.outbox.push_back(std::move(msg));
        pump_writes(c);
    }

    void pump_writes(Connection& c) {
        if (c.writing || !c.open || c.outbox.empty()) return;
        if (!take_outgoing_token()) {
            // Try again shortly; outgoing rate limit applies to control frames too.
            auto retry = std::make_shared<asio::steady_timer>(ioc);
            retry->expires_after(std::chrono::milliseconds(150));
            const int gen = c.generation;
            retry->async_wait([this, &c, gen, retry](const beast::error_code& ec) {
                if (!ec && c.generation == gen) pump_writes(c);
            });
            return;
        }
        c.writing = true;
        auto payload = std::make_shared<std::string>(std::move(c.outbox.front()));
        c.outbox.pop_front();
        const int gen = c.generation;
        c.ws->async_write(asio::buffer(*payload),
                          [this, &c, gen, payload](const beast::error_code& ec, size_t) {
                              if (c.generation != gen) return;
                              c.writing = false;
                              if (ec) {
                                  fail_connection(c, "write: " + ec.message());
                                  return;
                              }
                              pump_writes(c);
                          });
    }

    // ---- connection lifecycle ------------------------------------------------
    void start_connection(Connection& c) {
        if (stopping.load()) return;
        ++c.generation;
        const int gen = c.generation;
        c.open = false;
        c.writing = false;
        c.outbox.clear();
        c.awaiting_pong = false;
        c.ws = std::make_unique<WsStream>(ioc, ssl_ctx);

        auto resolver = std::make_shared<tcp::resolver>(ioc);
        resolver->async_resolve(
            url.host, url.port,
            [this, &c, gen, resolver](const beast::error_code& ec,
                                      const tcp::resolver::results_type& results) {
                if (c.generation != gen || stopping.load()) return;
                if (ec) {
                    fail_connection(c, "resolve: " + ec.message());
                    return;
                }
                beast::get_lowest_layer(*c.ws).expires_after(
                    std::chrono::milliseconds(cfg.connect_timeout_ms));
                beast::get_lowest_layer(*c.ws).async_connect(
                    results, [this, &c, gen](const beast::error_code& ec2,
                                             const tcp::endpoint&) {
                        if (c.generation != gen || stopping.load()) return;
                        if (ec2) {
                            fail_connection(c, "connect: " + ec2.message());
                            return;
                        }
                        if (SSL_set_tlsext_host_name(c.ws->next_layer().native_handle(),
                                                     url.host.c_str()) != 1) {
                            fail_connection(c, "SNI setup failed");
                            return;
                        }
                        SSL_set1_host(c.ws->next_layer().native_handle(), url.host.c_str());
                        c.ws->next_layer().async_handshake(
                            asio::ssl::stream_base::client,
                            [this, &c, gen](const beast::error_code& ec3) {
                                if (c.generation != gen || stopping.load()) return;
                                if (ec3) {
                                    fail_connection(c, "tls: " + ec3.message());
                                    return;
                                }
                                ws_handshake(c, gen);
                            });
                    });
            });
    }

    void ws_handshake(Connection& c, int gen) {
        beast::get_lowest_layer(*c.ws).expires_never();
        c.ws->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        c.ws->set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
            req.set(beast::http::field::user_agent, "altspotter/0.2");
        }));
        c.ws->async_handshake(url.host, url.path, [this, &c, gen](const beast::error_code& ec) {
            if (c.generation != gen || stopping.load()) return;
            if (ec) {
                fail_connection(c, "ws handshake: " + ec.message());
                return;
            }
            c.open = true;
            c.backoff.reset();
            c.last_inbound_mono_ms = mono_ms();
            log::info("ws_connected", {{"exchange", "bitget"},
                                       {"connection_id", c.id},
                                       {"symbols", c.symbols.size()}});
            if (callbacks.on_connection_event) {
                callbacks.on_connection_event("connected", c.id, "",
                                              static_cast<int32_t>(c.symbols.size()));
            }
            send_subscriptions(c);
            read_loop(c, gen);
            schedule_ping(c, gen);
            schedule_watchdog(c, gen);
            schedule_rotation(c, gen);
        });
    }

    void send_subscriptions(Connection& c) {
        json args = json::array();
        // Subscribe the depth channel only when a consumer wants book data
        // (the market gateway leaves on_orderbook null, so it is unaffected).
        const bool want_book = static_cast<bool>(callbacks.on_orderbook);
        for (const auto& sym : c.symbols) {
            args.push_back({{"instType", cfg.product_type}, {"channel", "trade"}, {"instId", sym}});
            args.push_back({{"instType", cfg.product_type}, {"channel", "candle1m"}, {"instId", sym}});
            if (want_book) {
                // Deeper book materially lifts predictive accuracy (Kumar et al.
                // 2025: 5-level ~54% vs deep-book ~71% at short horizons), so
                // subscribe books15 rather than books5. Consumer stores as many
                // levels as arrive.
                args.push_back(
                    {{"instType", cfg.product_type}, {"channel", "books15"}, {"instId", sym}});
            }
            if (static_cast<int32_t>(args.size()) >= cfg.subscribe_batch_size) {
                enqueue(c, json{{"op", "subscribe"}, {"args", args}}.dump());
                args = json::array();
            }
        }
        if (!args.empty()) {
            enqueue(c, json{{"op", "subscribe"}, {"args", args}}.dump());
        }
    }

    void read_loop(Connection& c, int gen) {
        auto buffer = std::make_shared<beast::flat_buffer>();
        read_one(c, gen, buffer);
    }

    void read_one(Connection& c, int gen, std::shared_ptr<beast::flat_buffer> buffer) {
        c.ws->async_read(*buffer, [this, &c, gen, buffer](const beast::error_code& ec, size_t) {
            if (c.generation != gen || stopping.load()) return;
            if (ec) {
                fail_connection(c, "read: " + ec.message());
                return;
            }
            c.last_inbound_mono_ms = mono_ms();
            const auto data = buffer->data();
            std::string_view text{static_cast<const char*>(data.data()), data.size()};
            // A malformed frame (unexpected JSON shape) must drop that frame,
            // not unwind through ioc.run() and terminate the whole process.
            try {
                handle_frame(c, text);
            } catch (const std::exception& e) {
                log::warn("ws_frame_handler_error",
                          {{"connection_id", c.id},
                           {"message", e.what()},
                           {"frame", std::string(text.substr(0, 120))}});
            }
            buffer->consume(buffer->size());
            read_one(c, gen, buffer);
        });
    }

    void handle_frame(Connection& c, std::string_view text) {
        const int64_t received_ms = timeutil::now_ms();
        if (text == "pong") {
            c.awaiting_pong = false;
            return;
        }
        json doc = json::parse(text, nullptr, false);
        if (doc.is_discarded() || !doc.is_object()) {
            log::warn("ws_unparseable_frame", {{"connection_id", c.id},
                                               {"message", std::string(text.substr(0, 120))}});
            return;
        }
        if (doc.contains("event")) {
            const std::string event = doc.value("event", "");
            if (event == "error") {
                log::error("ws_subscribe_error",
                           {{"connection_id", c.id},
                            {"error_code", doc.value("code", 0)},
                            {"message", doc.value("msg", "")}});
            } else if (event == "subscribe") {
                log::debug("ws_subscribed", {{"connection_id", c.id}});
            }
            return;
        }
        if (!doc.contains("arg") || !doc.contains("data")) return;

        const auto& arg = doc["arg"];
        const std::string channel = arg.value("channel", "");
        const std::string symbol = arg.value("instId", "");
        if (symbol.empty()) return;

        if (channel == "trade") {
            handle_trades(c, symbol, doc["data"], received_ms);
        } else if (channel == "candle1m") {
            handle_candles(c, symbol, doc["data"], received_ms);
        } else if (channel.rfind("books", 0) == 0) { // books, books5, books15
            handle_orderbook(c, symbol, doc["data"], received_ms);
        }
    }

    void handle_orderbook(Connection& c, const std::string& symbol, const json& data,
                          int64_t received_ms) {
        if (!data.is_array() || data.empty() || !callbacks.on_orderbook) return;
        const auto& snap = data.front();
        if (!snap.is_object()) return;
        OrderBook book;
        book.symbol = symbol;
        try { book.exchange_ms = std::stoll(snap.value("ts", "0")); }
        catch (const std::exception&) { book.exchange_ms = 0; }
        try { book.seq = std::stoll(snap.value("seq", "0")); }
        catch (const std::exception&) { book.seq = 0; }
        const auto load = [](const json& side, std::vector<OrderBookLevel>& out) {
            if (!side.is_array()) return;
            for (const auto& lvl : side) {
                if (!lvl.is_array() || lvl.size() < 2 || !lvl[0].is_string() || !lvl[1].is_string())
                    continue;
                out.push_back({lvl[0].get<std::string>(), lvl[1].get<std::string>()});
            }
        };
        load(snap.value("bids", json::array()), book.bids);
        load(snap.value("asks", json::array()), book.asks);
        if (book.bids.empty() || book.asks.empty()) return;
        callbacks.on_orderbook(symbol, book, received_ms, c.id);
    }

    void handle_trades(Connection& c, const std::string& symbol, const json& data,
                       int64_t received_ms) {
        if (!data.is_array() || !callbacks.on_trade) return;
        for (const auto& t : data) {
            if (!t.is_object()) continue;
            Trade trade;
            trade.exchange_trade_id = t.value("tradeId", "");
            const std::string side = t.value("side", "");
            trade.side = side == "buy" ? TradeSide::buy
                         : side == "sell" ? TradeSide::sell
                                          : TradeSide::unspecified;
            auto price = Decimal::parse(t.value("price", ""));
            auto size = Decimal::parse(t.value("size", ""));
            if (!price || !size) {
                log::warn("ws_trade_parse_error", {{"symbol", symbol}, {"connection_id", c.id}});
                continue;
            }
            trade.price = *price;
            trade.size = *size;
            try {
                trade.exchange_ts_ms = std::stoll(t.value("ts", "0"));
            } catch (const std::exception&) {
                trade.exchange_ts_ms = 0;
            }
            callbacks.on_trade(symbol, trade, received_ms, c.id);
        }
    }

    void handle_candles(Connection& c, const std::string& symbol, const json& data,
                        int64_t received_ms) {
        if (!data.is_array()) return;
        auto& st = c.state[symbol];
        for (const auto& row : data) {
            auto parsed = parse_ws_candle_row(row);
            if (!parsed) {
                log::warn("ws_candle_parse_error",
                          {{"symbol", symbol}, {"message", parsed.error().message}});
                continue;
            }
            Candle candle = std::move(*parsed);

            if (st.has_candle && candle.open_time_ms > st.last_candle.open_time_ms) {
                // Previous minute is now final: emit as closed (deduplicated).
                if (st.last_candle.open_time_ms > st.last_closed_open_ms) {
                    Candle closed = st.last_candle;
                    closed.is_closed = true;
                    st.last_closed_open_ms = closed.open_time_ms;
                    if (callbacks.on_candle_closed) {
                        callbacks.on_candle_closed(symbol, closed, received_ms, c.id);
                    }
                }
            }
            if (!st.has_candle || candle.open_time_ms >= st.last_candle.open_time_ms) {
                st.last_candle = candle;
                st.has_candle = true;
            }
            if (callbacks.on_candle_update) {
                callbacks.on_candle_update(symbol, candle, received_ms, c.id);
            }
        }
    }

    // ---- timers ---------------------------------------------------------------
    void schedule_ping(Connection& c, int gen) {
        c.ping_timer.expires_after(std::chrono::seconds(cfg.ws_ping_interval_sec));
        c.ping_timer.async_wait([this, &c, gen](const beast::error_code& ec) {
            if (ec || c.generation != gen || !c.open || stopping.load()) return;
            if (c.awaiting_pong &&
                mono_ms() - c.last_ping_sent_mono_ms >
                    static_cast<int64_t>(cfg.ws_pong_timeout_sec) * 1000) {
                fail_connection(c, "pong timeout");
                return;
            }
            c.awaiting_pong = true;
            c.last_ping_sent_mono_ms = mono_ms();
            enqueue(c, "ping");
            schedule_ping(c, gen);
        });
    }

    void schedule_watchdog(Connection& c, int gen) {
        c.watchdog_timer.expires_after(std::chrono::seconds(5));
        c.watchdog_timer.async_wait([this, &c, gen](const beast::error_code& ec) {
            if (ec || c.generation != gen || !c.open || stopping.load()) return;
            const int64_t idle = mono_ms() - c.last_inbound_mono_ms;
            if (idle > static_cast<int64_t>(cfg.stale_stream_timeout_sec) * 1000) {
                if (callbacks.on_connection_event) {
                    callbacks.on_connection_event("degraded", c.id, "stale stream",
                                                  static_cast<int32_t>(c.symbols.size()));
                }
                fail_connection(c, "stale stream (" + std::to_string(idle) + "ms idle)");
                return;
            }
            if (c.awaiting_pong &&
                mono_ms() - c.last_ping_sent_mono_ms >
                    static_cast<int64_t>(cfg.ws_pong_timeout_sec) * 1000) {
                fail_connection(c, "pong timeout");
                return;
            }
            schedule_watchdog(c, gen);
        });
    }

    void schedule_rotation(Connection& c, int gen) {
        // Stagger rotations so connections never rotate simultaneously.
        const int64_t base = static_cast<int64_t>(cfg.ws_rotation_interval_sec);
        const int64_t offset = base / std::max(1, n_connections) * c.index;
        c.rotation_timer.expires_after(std::chrono::seconds(base + offset));
        c.rotation_timer.async_wait([this, &c, gen](const beast::error_code& ec) {
            if (ec || c.generation != gen || !c.open || stopping.load()) return;
            log::info("ws_rotation", {{"connection_id", c.id}});
            fail_connection(c, "scheduled rotation", /*is_rotation=*/true);
        });
    }

    void fail_connection(Connection& c, const std::string& reason, bool is_rotation = false) {
        if (stopping.load()) return;
        const bool was_open = c.open;
        ++c.generation; // cancels all pending handlers of this session
        const int gen = c.generation;
        c.open = false;
        c.ping_timer.cancel();
        c.watchdog_timer.cancel();
        c.rotation_timer.cancel();
        if (c.ws) {
            beast::error_code ec;
            beast::get_lowest_layer(*c.ws).socket().close(ec);
        }
        if (was_open) {
            const auto fields =
                nlohmann::json{{"exchange", "bitget"},
                               {"connection_id", c.id}, {"message", reason}};
            if (is_rotation) log::info("ws_disconnected", fields);
            else log::warn("ws_disconnected", fields);
            if (callbacks.on_connection_event) {
                callbacks.on_connection_event("disconnected", c.id, reason,
                                              static_cast<int32_t>(c.symbols.size()));
            }
        }
        const auto delay = is_rotation ? std::chrono::milliseconds(200) : c.backoff.next();
        auto timer = std::make_shared<asio::steady_timer>(ioc);
        timer->expires_after(delay);
        timer->async_wait([this, &c, gen, timer](const beast::error_code& ec) {
            if (ec || stopping.load() || c.generation != gen) return;
            start_connection(c);
        });
    }
};

BitgetWsManager::BitgetWsManager(config::BitgetWsConfig cfg, int connections)
    : impl_(std::make_unique<Impl>(std::move(cfg), connections)) {}

BitgetWsManager::~BitgetWsManager() { stop(); }

void BitgetWsManager::start(const std::vector<std::string>& symbols, StreamCallbacks callbacks) {
    impl_->callbacks = std::move(callbacks);
    impl_->all_symbols = symbols;
    for (int i = 0; i < impl_->n_connections; ++i) {
        impl_->conns.push_back(std::make_unique<Impl::Connection>(impl_->ioc, i, impl_->cfg));
    }
    impl_->reshard();
    for (auto& c : impl_->conns) {
        impl_->start_connection(*c);
    }
    impl_->io_thread = std::thread([this] {
        auto guard = asio::make_work_guard(impl_->ioc);
        // Last line of defence: an exception escaping any completion handler
        // must not std::terminate the process. Log and resume the event loop.
        for (;;) {
            try {
                impl_->ioc.run();
                break;
            } catch (const std::exception& e) {
                log::error("ws_io_thread_exception", {{"message", e.what()}});
            }
        }
    });
}

void BitgetWsManager::update_symbols(const std::vector<std::string>& symbols) {
    asio::post(impl_->ioc, [this, symbols] {
        if (symbols == impl_->all_symbols) return;
        log::info("ws_symbols_updated", {{"count", symbols.size()}});
        impl_->all_symbols = symbols;
        impl_->reshard();
        // Simplest correct resubscribe: recycle each connection with new shards.
        for (auto& c : impl_->conns) {
            impl_->fail_connection(*c, "subscription update", /*is_rotation=*/true);
        }
    });
}

void BitgetWsManager::stop() {
    if (!impl_ || impl_->stopping.exchange(true)) return;
    asio::post(impl_->ioc, [this] {
        for (auto& c : impl_->conns) {
            ++c->generation;
            c->ping_timer.cancel();
            c->watchdog_timer.cancel();
            c->rotation_timer.cancel();
            if (c->ws && c->open) {
                beast::error_code ec;
                c->ws->close(websocket::close_code::normal, ec);
            } else if (c->ws) {
                beast::error_code ec;
                beast::get_lowest_layer(*c->ws).socket().close(ec);
            }
        }
        impl_->ioc.stop();
    });
    if (impl_->io_thread.joinable()) impl_->io_thread.join();
}

} // namespace altspotter::exchanges::bitget
