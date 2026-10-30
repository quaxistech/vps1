#include "altmacro/core/http.hpp"
#include "altmacro/core/shutdown.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace altmacro::core {
namespace {
std::string g_user_agent = "altmacro/1.0 (+https://altmacro.invalid)";
// libc getaddrinfo cannot reliably be cancelled once NSS is executing. An
// owned getent child keeps the SAME OS/NSS policy while making that worker
// killable. No detached threads, shell, alternative DNS policy or new daemon.
struct ResolverChild {
    pid_t pid = -1;
    ~ResolverChild() {
        if (pid <= 0) return;
        ::kill(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
};
struct PipeGuard {
    int* descriptors;
    ~PipeGuard() { for (int i = 0; i < 2; ++i) if (descriptors[i] >= 0) ::close(descriptors[i]); }
};
}

const std::string& default_user_agent() { return g_user_agent; }
void set_default_user_agent(std::string ua) { if (!ua.empty()) g_user_agent = std::move(ua); }

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;
using Clock = std::chrono::steady_clock;

struct HttpsClient::Impl {
    HttpOptions opts;
    net::io_context ioc;
    ssl::context ctx{ssl::context::tls_client};
    std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream;
    bool connected = false;

    explicit Impl(HttpOptions o) : opts(std::move(o)) {
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);
    }

    template<class Start, class Cancel>
    Result<void> phase(const char* name, Clock::time_point deadline, Start start, Cancel cancel) {
        if (ShutdownToken::instance().stop_requested())
            return fail(ErrorCode::shutdown_requested, "HTTPS cancelled");
        if (Clock::now() >= deadline) return fail(ErrorCode::timeout, "HTTPS deadline expired", true);
        ioc.restart();
        net::steady_timer timer(ioc);
        beast::error_code result;
        bool finished = false, expired = false, stopped = false;
        std::function<void()> watch;
        watch = [&] {
            timer.expires_at(std::min(deadline, Clock::now() + std::chrono::milliseconds(50)));
            timer.async_wait([&](beast::error_code ec) {
                if (ec || finished) return;
                stopped = ShutdownToken::instance().stop_requested();
                expired = Clock::now() >= deadline;
                if (stopped || expired) cancel();
                else watch();
            });
        };
        try {
            watch();
            start([&](beast::error_code ec) {
                result = ec; finished = true;
                beast::error_code ignored; timer.cancel(ignored);
            });
            // Drain ALL completions before referenced objects can die.
            ioc.run();
        } catch (...) {
            finished = true;
            beast::error_code ignored; timer.cancel(ignored);
            cancel(); ioc.restart(); ioc.run();
            throw;
        }
        if (stopped) return fail(ErrorCode::shutdown_requested, "HTTPS cancelled");
        if (expired) return fail(ErrorCode::timeout, std::string("HTTPS ") + name + " timed out", true);
        if (result) return fail(ErrorCode::network_error,
                                std::string("HTTPS ") + name + " failed: " + result.message(), true);
        return {};
    }

    void close_socket() noexcept {
        if (stream) { beast::error_code ec; beast::get_lowest_layer(*stream).socket().close(ec); }
    }
    void disconnect(bool graceful = false) noexcept {
        try {
            if (graceful && connected && stream) {
                (void)phase("shutdown", Clock::now() + std::chrono::milliseconds(1000),
                    [&](auto done) { stream->async_shutdown(std::move(done)); },
                    [&] { close_socket(); });
            }
        } catch (...) {} // teardown must still close after an allocation failure
        close_socket(); stream.reset(); connected = false;
    }

    Result<std::vector<tcp::endpoint>> resolve(Clock::time_point deadline) {
        unsigned port = 0;
        const auto parsed = std::from_chars(opts.port.data(), opts.port.data() + opts.port.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != opts.port.data() + opts.port.size() ||
            port == 0 || port > 65535)
            return fail(ErrorCode::invalid_argument, "HTTPS requires a numeric port in [1,65535]");
        beast::error_code ec;
        const auto literal = net::ip::make_address(opts.host, ec);
        if (!ec) return std::vector<tcp::endpoint>{{literal, static_cast<uint16_t>(port)}};

        int pipes[2]{-1, -1};
        if (::pipe2(pipes, O_CLOEXEC | O_NONBLOCK) != 0)
            return fail(ErrorCode::network_error, "cannot create resolver pipe", true);
        PipeGuard pipe_guard{pipes};
        net::posix::stream_descriptor input(ioc);
        input.assign(pipes[0], ec);
        if (ec) return fail(ErrorCode::network_error, "cannot assign resolver pipe", true);
        const int read_fd = pipes[0]; pipes[0] = -1;
        posix_spawn_file_actions_t actions;
        int error = posix_spawn_file_actions_init(&actions);
        if (error) return fail(ErrorCode::network_error, "resolver actions failed", true);
        error = posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
        if (!error) error = posix_spawn_file_actions_addclose(&actions, read_fd);
        if (!error) error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        if (!error) error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        ResolverChild child;
        char* const args[] = {const_cast<char*>("getent"), const_cast<char*>("--"),
                              const_cast<char*>("ahosts"), opts.host.data(), nullptr};
        pid_t spawned = -1;
        if (!error) error = ::posix_spawn(&spawned, "/usr/bin/getent", &actions, nullptr, args, ::environ);
        if (!error) child.pid = spawned;
        posix_spawn_file_actions_destroy(&actions);
        ::close(pipes[1]); pipes[1] = -1;
        if (error) return fail(ErrorCode::network_error, "resolver worker could not start", true);
        std::string output;
        std::array<char, 2048> buffer;
        std::function<void()> resolver_read;
        std::function<void(beast::error_code)> resolver_done;
        auto resolved = phase("resolve", deadline, [&](auto done) {
            resolver_done = std::move(done);
            resolver_read = [&] {
                input.async_read_some(net::buffer(buffer), [&](beast::error_code read_ec, size_t n) {
                    if (output.size() + n > 64 * 1024) { resolver_done(net::error::message_size); return; }
                    output.append(buffer.data(), n);
                    if (read_ec == net::error::eof) resolver_done({});
                    else if (read_ec) resolver_done(read_ec);
                    else resolver_read();
                });
            };
            resolver_read();
        }, [&] { beast::error_code ignored; input.close(ignored); ::kill(child.pid, SIGKILL); });
        resolver_read = {}; resolver_done = {};
        if (!resolved) return std::unexpected(resolved.error());
        std::vector<tcp::endpoint> endpoints;
        std::istringstream lines(output);
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream fields(line);
            std::string address, kind;
            fields >> address >> kind;
            auto ip = net::ip::make_address(address, ec);
            if (ec || kind != "STREAM") continue;
            tcp::endpoint endpoint(ip, static_cast<uint16_t>(port));
            if (std::find(endpoints.begin(), endpoints.end(), endpoint) == endpoints.end())
                endpoints.push_back(endpoint);
        }
        if (endpoints.empty()) return fail(ErrorCode::network_error, "DNS returned no TCP addresses", true);
        return endpoints;
    }
    Result<void> connect() {
        const auto deadline = Clock::now() + std::chrono::milliseconds(opts.connect_timeout_ms);
        auto endpoints = resolve(deadline);
        if (!endpoints) return std::unexpected(endpoints.error());
        stream = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(ioc, ctx);
        stream->set_verify_callback(ssl::host_name_verification(opts.host));
        if (!SSL_set_tlsext_host_name(stream->native_handle(), opts.host.c_str()))
            return fail(ErrorCode::network_error, "failed to set TLS SNI host");
        auto result = phase("connect", deadline, [&](auto done) {
            beast::get_lowest_layer(*stream).async_connect(*endpoints,
                [done](beast::error_code e, const tcp::endpoint&) { done(e); });
        }, [&] { close_socket(); });
        if (result) result = phase("TLS handshake", deadline,
            [&](auto done) { stream->async_handshake(ssl::stream_base::client, std::move(done)); },
            [&] { close_socket(); });
        if (!result) { disconnect(); return result; }
        connected = true;
        return {};
    }
};

HttpsClient::HttpsClient(HttpOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
HttpsClient::~HttpsClient() { if (impl_) impl_->disconnect(true); }
const std::string& HttpsClient::host() const noexcept { return impl_->opts.host; }

Result<HttpResponse> HttpsClient::get(const std::string& target) {
    if (target.empty() || target.front() != '/' || impl_->opts.connect_timeout_ms <= 0 ||
        impl_->opts.request_timeout_ms <= 0)
        return fail(ErrorCode::invalid_argument, "invalid HTTPS target or timeout");
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            if (!impl_->connected) {
                auto c = impl_->connect();
                if (!c) return std::unexpected(c.error());
            }
            http::request<http::empty_body> req{http::verb::get, target, 11};
            req.set(http::field::host, impl_->opts.host);
            req.set(http::field::user_agent, impl_->opts.user_agent.empty() ? default_user_agent() : impl_->opts.user_agent);
            req.set(http::field::accept, "*/*");
            const auto deadline = Clock::now() + std::chrono::milliseconds(impl_->opts.request_timeout_ms);
            auto result = impl_->phase("write", deadline, [&](auto done) {
                http::async_write(*impl_->stream, req, [done](beast::error_code e, size_t) { done(e); });
            }, [&] { impl_->close_socket(); });
            beast::flat_buffer buffer;
            http::response_parser<http::string_body> parser;
            parser.body_limit(256ull * 1024 * 1024);
            if (result) result = impl_->phase("read", deadline, [&](auto done) {
                http::async_read(*impl_->stream, buffer, parser, [done](beast::error_code e, size_t) { done(e); });
            }, [&] { impl_->close_socket(); });
            if (!result) {
                impl_->disconnect();
                if (attempt == 1 || result.error().code != ErrorCode::network_error)
                    return std::unexpected(result.error());
                continue;
            }
            auto response = parser.release();
            HttpResponse out;
            out.status = static_cast<int>(response.result_int()); out.body = std::move(response.body());
            for (const auto& field : response) {
                std::string name(field.name_string());
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                out.headers[name] = std::string(field.value());
            }
            if (response.need_eof() || !response.keep_alive()) impl_->disconnect();
            return out;
        } catch (const std::exception&) {
            impl_->disconnect();
            if (attempt == 1) return fail(ErrorCode::network_error, "HTTPS GET failed", true);
        }
    }
    return fail(ErrorCode::internal_error, "unreachable");
}
} // namespace altmacro::core
