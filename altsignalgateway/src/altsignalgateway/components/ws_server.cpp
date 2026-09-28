#include "altsignalgateway/components/ws_server.hpp"

#include "altsignalgateway/components/session.hpp"
#include "altsignalgateway/core/logger.hpp"

#include <openssl/ssl.h>
#include <sys/stat.h>

#include <boost/asio/post.hpp>

#include <filesystem>

namespace altsignalgateway::components {

namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
namespace pb = altsignalgateway::v1;

namespace {

// A hardened TLS 1.2 suite list, used only when min_version is lowered to 1.2.
// Forward secrecy and AEAD only: without both, a recorded session can be
// decrypted later from a leaked key, which for this endpoint means recovering
// every trade instruction VPS1 ever issued.
constexpr const char* kTls12Ciphers =
    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256";

// A private key readable beyond its owner and group is a finding whether or not
// it has leaked yet. Refuse to start rather than log a warning nobody reads.
Result<void> check_key_permissions(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return fail(ErrorCode::config_error, "cannot stat TLS private key: " + path);
    }
    if ((st.st_mode & (S_IROTH | S_IWOTH | S_IXOTH)) != 0) {
        return fail(ErrorCode::config_error,
                    "TLS private key is world-accessible (mode " +
                        std::to_string(st.st_mode & 07777) + "): " + path +
                        " — expected 0640 or stricter");
    }
    return {};
}

} // namespace

WsServer::WsServer(const config::RuntimeConfig& cfg, const domain::ClientRegistry& clients,
                   SessionRegistry& sessions, DeliveryPump& pump, QueryService& queries,
                   std::string server_version)
    : cfg_(cfg), clients_(clients), sessions_(sessions), pump_(pump), queries_(queries),
      server_version_(std::move(server_version)),
      io_(cfg.websocket.io_threads) {}

WsServer::~WsServer() { stop(); }

Result<void> WsServer::prepare() {
    const auto& t = cfg_.tls;
    std::error_code fs_ec;
    if (!std::filesystem::exists(t.certificate_chain_file, fs_ec)) {
        return fail(ErrorCode::config_error,
                    "TLS certificate chain not found: " + t.certificate_chain_file);
    }
    if (!std::filesystem::exists(t.private_key_file, fs_ec)) {
        return fail(ErrorCode::config_error, "TLS private key not found: " + t.private_key_file);
    }
    if (auto r = check_key_permissions(t.private_key_file); !r) return r;

    ssl_ = std::make_unique<ssl::context>(ssl::context::tls_server);
    try {
        // Every legacy protocol is refused unconditionally. `no_tlsv1_2` is
        // added on top when the configuration asks for TLS 1.3 only.
        auto options = ssl::context::default_workarounds | ssl::context::no_sslv2 |
                       ssl::context::no_sslv3 | ssl::context::no_tlsv1 |
                       ssl::context::no_tlsv1_1 | ssl::context::single_dh_use;
        if (t.min_version == "1.3") options |= ssl::context::no_tlsv1_2;
        ssl_->set_options(options);

        ssl_->use_certificate_chain_file(t.certificate_chain_file);
        ssl_->use_private_key_file(t.private_key_file, ssl::context::pem);
        if (!t.dh_params_file.empty()) ssl_->use_tmp_dh_file(t.dh_params_file);

        if (!t.cipher_list_tls12.empty()) {
            SSL_CTX_set_cipher_list(ssl_->native_handle(), t.cipher_list_tls12.c_str());
        } else if (t.min_version == "1.2") {
            SSL_CTX_set_cipher_list(ssl_->native_handle(), kTls12Ciphers);
        }
        if (!t.ciphersuites_tls13.empty()) {
            SSL_CTX_set_ciphersuites(ssl_->native_handle(), t.ciphersuites_tls13.c_str());
        }
        // The server picks the suite, not the client: otherwise a client can
        // steer the connection to the weakest mutually supported option.
        SSL_CTX_set_options(ssl_->native_handle(), SSL_OP_CIPHER_SERVER_PREFERENCE);
        // Session tickets add a second place a session key can be recovered
        // from, for no benefit on a link that carries one long-lived
        // connection.
        SSL_CTX_set_options(ssl_->native_handle(), SSL_OP_NO_TICKET);

        if (t.require_client_certificate) {
            ssl_->load_verify_file(t.client_ca_file);
            // fail_if_no_peer_cert matters: without it, verify_peer accepts a
            // client that simply presents no certificate at all.
            ssl_->set_verify_mode(ssl::verify_peer | ssl::verify_fail_if_no_peer_cert);
        } else {
            ssl_->set_verify_mode(ssl::verify_none);
        }
    } catch (const std::exception& e) {
        return fail(ErrorCode::config_error, std::string("TLS context setup failed: ") + e.what());
    }

    try {
        const auto address = asio::ip::make_address(cfg_.websocket.listen_address);
        const tcp::endpoint endpoint{address, cfg_.websocket.listen_port};
        acceptor_ = std::make_unique<tcp::acceptor>(io_);
        acceptor_->open(endpoint.protocol());
        acceptor_->set_option(asio::socket_base::reuse_address(true));
        acceptor_->bind(endpoint);
        acceptor_->listen(cfg_.websocket.accept_backlog);
        bound_port_.store(acceptor_->local_endpoint().port(), std::memory_order_release);
    } catch (const std::exception& e) {
        return fail(ErrorCode::network_error,
                    "cannot listen on " + cfg_.websocket.listen_address + ":" +
                        std::to_string(cfg_.websocket.listen_port) + ": " + e.what());
    }

    log::info("websocket_listener_ready",
              {{"address", cfg_.websocket.listen_address},
               {"port", static_cast<int>(bound_port_.load(std::memory_order_acquire))},
               {"tls_min_version", t.min_version},
               {"mutual_tls", t.require_client_certificate},
               {"max_clients", cfg_.websocket.max_clients},
               {"authorised_clients", clients_.size()}});
    return {};
}

void WsServer::start() {
    listening_.store(true, std::memory_order_release);
    accept_next();
    threads_.reserve(static_cast<size_t>(cfg_.websocket.io_threads));
    for (int32_t i = 0; i < cfg_.websocket.io_threads; ++i) {
        threads_.emplace_back([this] {
            log::set_thread_component("websocket");
            io_.run();
        });
    }
}

void WsServer::accept_next() {
    if (stopping_.load(std::memory_order_acquire)) return;
    acceptor_->async_accept(asio::make_strand(io_), [this](boost::system::error_code ec,
                                                           tcp::socket socket) {
        if (stopping_.load(std::memory_order_acquire)) return;
        if (ec) {
            // A failed accept must never end the accept loop: a transient
            // EMFILE or a client that vanished between SYN and accept would
            // otherwise take the endpoint down until the next restart.
            if (ec != asio::error::operation_aborted) {
                log::warn("accept_failed", {{"error", ec.message()}});
            }
            accept_next();
            return;
        }
        try {
            socket.set_option(tcp::no_delay(true));
        } catch (const std::exception&) {
            // Nagle stays on; a signal arrives a few milliseconds later. Not
            // worth refusing the connection over.
        }

        SessionDeps deps;
        deps.ws = cfg_.websocket;
        deps.auth = cfg_.auth;
        deps.query = cfg_.query;
        deps.run_query = [this](QueryJob job) { return queries_.submit(std::move(job)); };
        deps.registry = &clients_;
        deps.server_version = server_version_;
        deps.submit = [this](JournalOp op) { pump_.submit(std::move(op)); };
        deps.load_cursor = [this](std::string client_id,
                                  std::function<void(int64_t, int64_t)> reply) {
            pump_.query_cursor(std::move(client_id), std::move(reply));
        };
        deps.query_backlog = [this](int64_t from, std::function<void(int64_t, int64_t)> reply) {
            pump_.query_backlog(from, std::move(reply));
        };
        deps.attach = [this](std::shared_ptr<Session> s) { return sessions_.attach(s); };
        deps.detach = [this](const std::string& id) { sessions_.detach(id); };
        deps.is_locked_out = [this](const std::string& peer) {
            return sessions_.is_locked_out(peer);
        };
        deps.note_auth_failure = [this](const std::string& peer) {
            sessions_.note_auth_failure(peer);
        };
        deps.note_auth_success = [this](const std::string& peer) {
            sessions_.note_auth_success(peer);
        };
        deps.enforce_client_limit = [this](const std::string& client_id,
                                           const std::string& keep, int32_t max_sessions) {
            for (const auto& old : sessions_.enforce_client_limit(client_id, keep, max_sessions)) {
                log::info("session_superseded", {{"client_id", client_id},
                                                 {"superseded_session_id", old->session_id()},
                                                 {"by_session_id", keep}});
                old->terminate(pb::ERR_SESSION_SUPERSEDED,
                               "a newer session for this client_id has authenticated");
            }
        };
        deps.wake_pump = [this] { pump_.wake(); };

        std::make_shared<Session>(std::move(socket), *ssl_, std::move(deps))->run();
        accept_next();
    });
}

void WsServer::stop() {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) return;
    listening_.store(false, std::memory_order_release);

    // Stop accepting first, so nothing new arrives while the live sessions are
    // being told to go.
    if (acceptor_ != nullptr) {
        boost::system::error_code ec;
        acceptor_->close(ec);
    }
    for (const auto& session : sessions_.snapshot()) {
        session->terminate(pb::ERR_SERVER_SHUTTING_DOWN, "gateway is shutting down");
    }
    // Give the close frames a chance to leave before the reactor is torn down.
    // Anything still in flight is redelivered after the restart: the journal is
    // the record, not the socket.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(cfg_.websocket.shutdown_drain_ms);
    while (sessions_.size() > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    io_.stop();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
    log::info("websocket_listener_stopped", {});
}

} // namespace altsignalgateway::components
