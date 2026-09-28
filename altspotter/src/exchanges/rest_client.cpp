#include "exchanges/rest_client.hpp"

#include "logging/logger.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include <thread>

namespace altspotter::exchanges {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

struct HttpsClient::Impl {
    RestClientOptions opts;
    asio::io_context ioc;
    asio::ssl::context ssl_ctx{asio::ssl::context::tls_client};
    std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream;

    explicit Impl(RestClientOptions o) : opts(std::move(o)) {
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(asio::ssl::verify_peer); // TLS verification is mandatory
        ssl_ctx.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                            asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                            asio::ssl::context::no_tlsv1_1);
    }

    void disconnect() noexcept {
        if (stream) {
            beast::error_code ec;
            beast::get_lowest_layer(*stream).socket().shutdown(tcp::socket::shutdown_both, ec);
            beast::get_lowest_layer(*stream).close();
            stream.reset();
        }
    }

    Result<void> ensure_connected() {
        if (stream) return {};
        try {
            tcp::resolver resolver(ioc);
            auto endpoints = resolver.resolve(opts.host, opts.port);

            stream = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(ioc, ssl_ctx);
            if (SSL_set_tlsext_host_name(stream->native_handle(), opts.host.c_str()) != 1) {
                stream.reset();
                return fail(ErrorCode::network_error, "failed to set SNI hostname");
            }
            SSL_set1_host(stream->native_handle(), opts.host.c_str()); // hostname verification

            beast::get_lowest_layer(*stream).expires_after(
                std::chrono::milliseconds(opts.connect_timeout_ms));
            beast::get_lowest_layer(*stream).connect(endpoints);
            stream->handshake(asio::ssl::stream_base::client);
            return {};
        } catch (const std::exception& e) {
            stream.reset();
            return fail(ErrorCode::network_error,
                        "connect " + opts.host + ": " + e.what(), true);
        }
    }
};

HttpsClient::HttpsClient(RestClientOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
HttpsClient::~HttpsClient() {
    if (impl_) impl_->disconnect();
}

Result<HttpResponse> HttpsClient::get(const std::string& target) {
    return request("GET", target, {});
}

Result<HttpResponse> HttpsClient::request(
    const std::string& method, const std::string& target, std::string body,
    const std::vector<std::pair<std::string, std::string>>& headers) {
    if (target.empty() || target.front() != '/') {
        return fail(ErrorCode::invalid_argument, "HTTP target must be origin-form");
    }
    http::verb verb = http::string_to_verb(method);
    if (verb == http::verb::unknown) {
        return fail(ErrorCode::invalid_argument, "unsupported HTTP method");
    }
    for (int attempt_on_stale = 0; attempt_on_stale < 2; ++attempt_on_stale) {
        if (auto r = impl_->ensure_connected(); !r) return std::unexpected(r.error());
        try {
            http::request<http::string_body> req{verb, target, 11};
            req.set(http::field::host, impl_->opts.host);
            req.set(http::field::user_agent, impl_->opts.user_agent);
            req.set(http::field::accept, "application/json");
            for (const auto& [name, value] : headers) req.set(name, value);
            // Preserve the canonical signed body for the one stale-connection
            // retry; moving it would turn a retried POST into a different
            // request and invalidate the exchange signature.
            req.body() = body;
            req.prepare_payload();

            beast::get_lowest_layer(*impl_->stream)
                .expires_after(std::chrono::milliseconds(impl_->opts.request_timeout_ms));
            http::write(*impl_->stream, req);

            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(*impl_->stream, buffer, res);

            if (res.need_eof() || res[http::field::connection] == "close") {
                impl_->disconnect();
            }
            return HttpResponse{static_cast<int>(res.result_int()), std::move(res.body())};
        } catch (const beast::system_error& e) {
            impl_->disconnect();
            // A stale keep-alive connection commonly fails on the first write/read;
            // retry once with a fresh connection before reporting an error.
            if (attempt_on_stale == 0) continue;
            const bool timeout = e.code() == beast::error::timeout;
            return fail(timeout ? ErrorCode::timeout : ErrorCode::network_error,
                        method + " " + target + ": " + e.code().message(), true);
        } catch (const std::exception& e) {
            impl_->disconnect();
            return fail(ErrorCode::network_error, method + " failed: " + e.what(), true);
        }
    }
    return fail(ErrorCode::network_error, method + " " + target + ": unreachable", true);
}

Result<HttpResponse> get_with_retry(HttpsClient& client, TokenBucket& bucket,
                                    const std::string& target, int32_t max_retries,
                                    int32_t backoff_min_ms, int32_t backoff_max_ms,
                                    const std::function<bool()>& should_stop) {
    Backoff backoff{std::chrono::milliseconds(backoff_min_ms),
                    std::chrono::milliseconds(backoff_max_ms)};
    Error last_error{ErrorCode::internal_error, "no attempts made", false};

    for (int32_t attempt = 0; attempt <= max_retries; ++attempt) {
        if (should_stop()) return fail(ErrorCode::shutdown_requested, "shutdown requested");
        if (!bucket.acquire(should_stop)) {
            return fail(ErrorCode::shutdown_requested, "shutdown while rate limited");
        }

        auto res = client.get(target);
        if (res) {
            if (res->status == 429 || (res->status >= 500 && res->status < 600)) {
                last_error = Error{res->status == 429 ? ErrorCode::rate_limited
                                                      : ErrorCode::upstream_error,
                                   "HTTP " + std::to_string(res->status) + " for " + target, true};
                log::warn("rest_retryable_status",
                          {{"error_code", res->status}, {"retry_count", attempt}});
            } else {
                return res;
            }
        } else {
            last_error = res.error();
            if (!last_error.retryable) return std::unexpected(last_error);
            log::warn("rest_network_retry", {{"message", last_error.message},
                                             {"retry_count", attempt}});
        }

        if (attempt < max_retries) {
            const auto delay = backoff.next();
            const auto deadline = std::chrono::steady_clock::now() + delay;
            while (std::chrono::steady_clock::now() < deadline) {
                if (should_stop()) return fail(ErrorCode::shutdown_requested, "shutdown in backoff");
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }
    last_error.message += " (retries exhausted)";
    return std::unexpected(last_error);
}

} // namespace altspotter::exchanges
