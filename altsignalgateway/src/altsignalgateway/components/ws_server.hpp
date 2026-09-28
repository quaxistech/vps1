#pragma once

#include "altsignalgateway/components/delivery_pump.hpp"
#include "altsignalgateway/components/query_service.hpp"
#include "altsignalgateway/components/session_registry.hpp"
#include "altsignalgateway/core/config.hpp"
#include "altsignalgateway/core/error.hpp"
#include "altsignalgateway/domain/auth.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace altsignalgateway::components {

// The TLS WebSocket listener: VPS1's external boundary.
//
// It owns the OpenSSL context, the acceptor and a small pool of I/O threads.
// It owns no trading state at all — a session that has authenticated can
// receive, acknowledge and resume, and there is no frame in the protocol that
// writes anything else into VPS1.
class WsServer {
public:
    WsServer(const config::RuntimeConfig& cfg, const domain::ClientRegistry& clients,
             SessionRegistry& sessions, DeliveryPump& pump, QueryService& queries,
             std::string server_version);
    ~WsServer();

    // Build the SSL context and bind the acceptor. Reports a readable error
    // rather than throwing, so a bad certificate path stops the service at
    // startup with a message an operator can act on.
    Result<void> prepare();
    void start();
    // Stop accepting, close every live session, join the I/O threads.
    void stop();

    [[nodiscard]] bool listening() const { return listening_.load(std::memory_order_acquire); }
    [[nodiscard]] uint16_t bound_port() const { return bound_port_.load(std::memory_order_acquire); }

private:
    void accept_next();

    config::RuntimeConfig cfg_;
    const domain::ClientRegistry& clients_;
    SessionRegistry& sessions_;
    DeliveryPump& pump_;
    QueryService& queries_;
    std::string server_version_;

    boost::asio::io_context io_;
    std::unique_ptr<boost::asio::ssl::context> ssl_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::vector<std::thread> threads_;
    std::atomic<bool> listening_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<uint16_t> bound_port_{0};
};

} // namespace altsignalgateway::components
