#include "exchanges/venue_ws.hpp"

#include "core/backoff.hpp"
#include "core/time.hpp"
#include "logging/logger.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <atomic>
#include <deque>
#include <thread>

namespace altspotter::exchanges {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

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

int64_t mono_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

struct VenueWsSession::Impl {
    using WsStream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    std::string conn_id;
    std::unique_ptr<VenueWsProtocol> protocol;
    Options opts;
    UrlParts url;

    asio::io_context ioc;
    asio::ssl::context ssl_ctx{asio::ssl::context::tls_client};
    std::unique_ptr<WsStream> ws;
    std::vector<std::string> symbols;
    Callbacks callbacks;

    std::deque<std::string> outbox;
    bool writing = false;
    std::atomic<bool> open{false};
    std::atomic<bool> stopping{false};
    int generation = 0;
    int64_t last_inbound_mono_ms = 0;
    bool awaiting_pong = false;
    int64_t last_ping_sent_mono_ms = 0;
    Backoff backoff;
    asio::steady_timer ping_timer;
    asio::steady_timer watchdog_timer;
    asio::steady_timer rotation_timer;
    std::thread io_thread;

    Impl(std::string id, std::unique_ptr<VenueWsProtocol> proto, Options o)
        : conn_id(std::move(id)), protocol(std::move(proto)), opts(o),
          backoff(std::chrono::milliseconds(o.backoff_min_ms),
                  std::chrono::milliseconds(o.backoff_max_ms)),
          ping_timer(ioc), watchdog_timer(ioc), rotation_timer(ioc) {
        url = parse_wss_url(protocol->ws_url());
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(asio::ssl::verify_peer);
    }

    // ---- outgoing ----------------------------------------------------------
    void enqueue(std::string frame) {
        outbox.push_back(std::move(frame));
        pump_writes();
    }

    void pump_writes() {
        if (writing || !open.load() || outbox.empty()) return;
        writing = true;
        auto payload = std::make_shared<std::string>(std::move(outbox.front()));
        outbox.pop_front();
        const int gen = generation;
        ws->async_write(asio::buffer(*payload),
                        [this, gen, payload](const beast::error_code& ec, size_t) {
                            if (generation != gen) return;
                            writing = false;
                            if (ec) {
                                fail("write: " + ec.message());
                                return;
                            }
                            pump_writes();
                        });
    }

    // ---- lifecycle ---------------------------------------------------------
    void start_connection() {
        if (stopping.load()) return;
        ++generation;
        const int gen = generation;
        open.store(false);
        writing = false;
        outbox.clear();
        awaiting_pong = false;
        ws = std::make_unique<WsStream>(ioc, ssl_ctx);

        auto resolver = std::make_shared<tcp::resolver>(ioc);
        resolver->async_resolve(
            url.host, url.port,
            [this, gen, resolver](const beast::error_code& ec,
                                  const tcp::resolver::results_type& results) {
                if (generation != gen || stopping.load()) return;
                if (ec) {
                    fail("resolve: " + ec.message());
                    return;
                }
                beast::get_lowest_layer(*ws).expires_after(
                    std::chrono::milliseconds(opts.connect_timeout_ms));
                beast::get_lowest_layer(*ws).async_connect(
                    results,
                    [this, gen](const beast::error_code& ec2, const tcp::endpoint&) {
                        if (generation != gen || stopping.load()) return;
                        if (ec2) {
                            fail("connect: " + ec2.message());
                            return;
                        }
                        if (SSL_set_tlsext_host_name(ws->next_layer().native_handle(),
                                                     url.host.c_str()) != 1) {
                            fail("SNI setup failed");
                            return;
                        }
                        SSL_set1_host(ws->next_layer().native_handle(), url.host.c_str());
                        ws->next_layer().async_handshake(
                            asio::ssl::stream_base::client,
                            [this, gen](const beast::error_code& ec3) {
                                if (generation != gen || stopping.load()) return;
                                if (ec3) {
                                    fail("tls: " + ec3.message());
                                    return;
                                }
                                ws_handshake(gen);
                            });
                    });
            });
    }

    void ws_handshake(int gen) {
        beast::get_lowest_layer(*ws).expires_never();
        ws->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws->set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
            req.set(beast::http::field::user_agent, "altspotter/0.2");
        }));
        // Protocol-level ping/pong (Binance) is answered by Beast automatically;
        // observing control frames keeps the stale watchdog accurate.
        ws->control_callback([this](websocket::frame_type, beast::string_view) {
            last_inbound_mono_ms = mono_ms();
        });
        ws->async_handshake(url.host, url.path, [this, gen](const beast::error_code& ec) {
            if (generation != gen || stopping.load()) return;
            if (ec) {
                fail("ws handshake: " + ec.message());
                return;
            }
            open.store(true);
            backoff.reset();
            last_inbound_mono_ms = mono_ms();
            log::info("venue_ws_connected",
                      {{"connection_id", conn_id}, {"symbols", symbols.size()}});
            if (callbacks.on_connection_event) {
                callbacks.on_connection_event("connected", conn_id, "");
            }
            for (auto& frame : protocol->subscribe_frames(symbols)) {
                enqueue(std::move(frame));
            }
            read_loop(gen);
            schedule_ping(gen);
            schedule_watchdog(gen);
            schedule_rotation(gen);
        });
    }

    void read_loop(int gen) {
        auto buffer = std::make_shared<beast::flat_buffer>();
        read_one(gen, buffer);
    }

    void read_one(int gen, std::shared_ptr<beast::flat_buffer> buffer) {
        ws->async_read(*buffer, [this, gen, buffer](const beast::error_code& ec, size_t) {
            if (generation != gen || stopping.load()) return;
            if (ec) {
                fail("read: " + ec.message());
                return;
            }
            last_inbound_mono_ms = mono_ms();
            const auto data = buffer->data();
            std::string_view text{static_cast<const char*>(data.data()), data.size()};
            const int64_t recv_ms = timeutil::now_ms();
            try {
                VenueEvents events;
                if (!protocol->parse(text, events)) {
                    log::warn("venue_ws_unparseable_frame",
                              {{"connection_id", conn_id},
                               {"frame", std::string(text.substr(0, 160))}});
                } else {
                    if (events.is_pong) awaiting_pong = false;
                    if (events.protocol_error) {
                        log::error("venue_ws_protocol_error",
                                   {{"connection_id", conn_id},
                                    {"message", *events.protocol_error}});
                    }
                    if (callbacks.on_events &&
                        (!events.trades.empty() || !events.candles.empty() ||
                         !events.tickers.empty() || !events.books.empty())) {
                        callbacks.on_events(std::move(events), recv_ms, conn_id);
                    }
                }
            } catch (const std::exception& e) {
                // A malformed frame must never unwind the io thread.
                log::warn("venue_ws_frame_handler_error",
                          {{"connection_id", conn_id},
                           {"message", e.what()},
                           {"frame", std::string(text.substr(0, 160))}});
            }
            buffer->consume(buffer->size());
            read_one(gen, buffer);
        });
    }

    // ---- timers ------------------------------------------------------------
    void schedule_ping(int gen) {
        const int interval = protocol->ping_interval_sec();
        if (interval <= 0 || protocol->ping_frame().empty()) return;
        ping_timer.expires_after(std::chrono::seconds(interval));
        ping_timer.async_wait([this, gen](const beast::error_code& ec) {
            if (ec || generation != gen || !open.load() || stopping.load()) return;
            if (awaiting_pong &&
                mono_ms() - last_ping_sent_mono_ms > 2LL * protocol->ping_interval_sec() * 1000) {
                fail("pong timeout");
                return;
            }
            awaiting_pong = true;
            last_ping_sent_mono_ms = mono_ms();
            enqueue(protocol->ping_frame());
            schedule_ping(gen);
        });
    }

    void schedule_watchdog(int gen) {
        watchdog_timer.expires_after(std::chrono::seconds(5));
        watchdog_timer.async_wait([this, gen](const beast::error_code& ec) {
            if (ec || generation != gen || !open.load() || stopping.load()) return;
            const int64_t idle = mono_ms() - last_inbound_mono_ms;
            if (idle > static_cast<int64_t>(opts.stale_timeout_sec) * 1000) {
                if (callbacks.on_connection_event) {
                    callbacks.on_connection_event("degraded", conn_id, "stale stream");
                }
                fail("stale stream (" + std::to_string(idle) + "ms idle)");
                return;
            }
            schedule_watchdog(gen);
        });
    }

    void schedule_rotation(int gen) {
        const int interval = protocol->rotation_interval_sec();
        if (interval <= 0) return;
        rotation_timer.expires_after(std::chrono::seconds(interval));
        rotation_timer.async_wait([this, gen](const beast::error_code& ec) {
            if (ec || generation != gen || !open.load() || stopping.load()) return;
            log::info("venue_ws_rotation", {{"connection_id", conn_id}});
            fail("scheduled rotation", /*is_rotation=*/true);
        });
    }

    void fail(const std::string& reason, bool is_rotation = false) {
        if (stopping.load()) return;
        const bool was_open = open.load();
        ++generation;
        const int gen = generation;
        open.store(false);
        ping_timer.cancel();
        watchdog_timer.cancel();
        rotation_timer.cancel();
        if (ws) {
            beast::error_code ec;
            beast::get_lowest_layer(*ws).socket().close(ec);
        }
        if (was_open) {
            if (is_rotation) {
                log::info("venue_ws_disconnected", {{"connection_id", conn_id}, {"message", reason}});
            } else {
                log::warn("venue_ws_disconnected", {{"connection_id", conn_id}, {"message", reason}});
            }
            if (callbacks.on_connection_event) {
                callbacks.on_connection_event("disconnected", conn_id, reason);
            }
        }
        const auto delay = is_rotation ? std::chrono::milliseconds(200) : backoff.next();
        auto timer = std::make_shared<asio::steady_timer>(ioc);
        timer->expires_after(delay);
        timer->async_wait([this, gen, timer](const beast::error_code& ec) {
            if (ec || stopping.load() || generation != gen) return;
            start_connection();
        });
    }
};

VenueWsSession::VenueWsSession(std::string conn_id, std::unique_ptr<VenueWsProtocol> protocol,
                               Options options)
    : impl_(std::make_unique<Impl>(std::move(conn_id), std::move(protocol), options)) {}

VenueWsSession::~VenueWsSession() { stop(); }

void VenueWsSession::start(const std::vector<std::string>& symbols, Callbacks callbacks) {
    impl_->symbols = symbols;
    impl_->callbacks = std::move(callbacks);
    impl_->start_connection();
    impl_->io_thread = std::thread([impl = impl_.get()] {
        auto guard = asio::make_work_guard(impl->ioc);
        for (;;) {
            try {
                impl->ioc.run();
                break;
            } catch (const std::exception& e) {
                log::error("venue_ws_io_exception",
                           {{"connection_id", impl->conn_id}, {"message", e.what()}});
            }
        }
    });
}

void VenueWsSession::update_symbols(const std::vector<std::string>& symbols) {
    asio::post(impl_->ioc, [this, symbols] {
        if (symbols == impl_->symbols) return;
        impl_->symbols = symbols;
        log::info("venue_ws_symbols_updated",
                  {{"connection_id", impl_->conn_id}, {"count", symbols.size()}});
        // Simplest correct resubscribe: recycle the connection with the new set.
        impl_->fail("subscription update", /*is_rotation=*/true);
    });
}

void VenueWsSession::send_frames(std::vector<std::string> frames) {
    asio::post(impl_->ioc, [this, frames = std::move(frames)]() mutable {
        if (!impl_->open.load()) return;
        for (auto& f : frames) impl_->enqueue(std::move(f));
    });
}

void VenueWsSession::stop() {
    if (!impl_ || impl_->stopping.exchange(true)) return;
    asio::post(impl_->ioc, [this] {
        ++impl_->generation;
        impl_->ping_timer.cancel();
        impl_->watchdog_timer.cancel();
        impl_->rotation_timer.cancel();
        if (impl_->ws) {
            beast::error_code ec;
            if (impl_->open.load()) {
                impl_->ws->close(websocket::close_code::normal, ec);
            } else {
                beast::get_lowest_layer(*impl_->ws).socket().close(ec);
            }
        }
        impl_->ioc.stop();
    });
    if (impl_->io_thread.joinable()) impl_->io_thread.join();
}

bool VenueWsSession::connected() const { return impl_->open.load(); }

} // namespace altspotter::exchanges
