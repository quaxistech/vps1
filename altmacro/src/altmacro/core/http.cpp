#include "altmacro/core/http.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include <algorithm>
#include <cctype>

namespace altmacro::core {
namespace {
std::string g_user_agent = "altmacro/1.0 (+https://altmacro.invalid)";
}

const std::string& default_user_agent() { return g_user_agent; }
void set_default_user_agent(std::string ua) {
    if (!ua.empty()) g_user_agent = std::move(ua);
}

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

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

    void disconnect() noexcept {
        if (stream) {
            beast::error_code ec;
            stream->shutdown(ec);
            beast::get_lowest_layer(*stream).close();
        }
        stream.reset();
        connected = false;
    }

    Result<void> connect() {
        try {
            stream = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(ioc, ctx);
            // SNI is mandatory on every host we talk to, and its absence shows
            // up as a confusing handshake failure rather than a clear error.
            if (!SSL_set_tlsext_host_name(stream->native_handle(), opts.host.c_str())) {
                return fail(ErrorCode::network_error, "failed to set TLS SNI host");
            }
            tcp::resolver resolver(ioc);
            const auto endpoints = resolver.resolve(opts.host, opts.port);
            beast::get_lowest_layer(*stream).expires_after(
                std::chrono::milliseconds(opts.connect_timeout_ms));
            beast::get_lowest_layer(*stream).connect(endpoints);
            stream->handshake(ssl::stream_base::client);
            connected = true;
            return {};
        } catch (const std::exception& e) {
            disconnect();
            return fail(ErrorCode::network_error,
                        "connect to " + opts.host + " failed: " + e.what(), true);
        }
    }
};

HttpsClient::HttpsClient(HttpOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
HttpsClient::~HttpsClient() {
    if (impl_) impl_->disconnect();
}

const std::string& HttpsClient::host() const noexcept { return impl_->opts.host; }

Result<HttpResponse> HttpsClient::get(const std::string& target) {
    if (target.empty() || target.front() != '/') {
        return fail(ErrorCode::invalid_argument, "target must be origin-form and begin with '/'");
    }

    // One transparent retry: a kept-alive connection can be closed by the peer
    // between calls, which surfaces as an error on write rather than at idle.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!impl_->connected) {
            auto c = impl_->connect();
            if (!c) return std::unexpected(c.error());
        }
        try {
            http::request<http::empty_body> req{http::verb::get, target, 11};
            req.set(http::field::host, impl_->opts.host);
            req.set(http::field::user_agent, impl_->opts.user_agent.empty()
                                                 ? default_user_agent()
                                                 : impl_->opts.user_agent);
            req.set(http::field::accept, "*/*");

            beast::get_lowest_layer(*impl_->stream)
                .expires_after(std::chrono::milliseconds(impl_->opts.request_timeout_ms));
            http::write(*impl_->stream, req);

            beast::flat_buffer buffer;
            // Bodies here are whole CSV series and compressed archives, so the
            // default 8 MB limit is raised deliberately rather than by accident.
            http::response_parser<http::string_body> parser;
            parser.body_limit(256ull * 1024 * 1024);
            http::read(*impl_->stream, buffer, parser);
            auto res = parser.release();

            HttpResponse out;
            out.status = static_cast<int>(res.result_int());
            out.body = std::move(res.body());
            for (const auto& field : res) {
                std::string name(field.name_string());
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                out.headers[name] = std::string(field.value());
            }
            if (res.need_eof() || !res.keep_alive()) impl_->disconnect();
            return out;
        } catch (const std::exception& e) {
            impl_->disconnect();
            if (attempt == 1) {
                return fail(ErrorCode::network_error,
                            "GET " + impl_->opts.host + target + " failed: " + e.what(), true);
            }
        }
    }
    return fail(ErrorCode::internal_error, "unreachable");
}

} // namespace altmacro::core
