#pragma once

#include "core/backoff.hpp"
#include "core/error.hpp"
#include "core/rate_limiter.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace altspotter::exchanges {

struct HttpResponse {
    int status = 0;
    std::string body;
};

struct RestClientOptions {
    std::string host;               // api.bitget.com
    std::string port = "443";
    int32_t connect_timeout_ms = 5000;
    int32_t request_timeout_ms = 10000;
    std::string user_agent = "altspotter/0.2";
};

// Synchronous HTTPS/1.1 client (Boost.Beast + OpenSSL, certificate
// verification enforced). Keeps the connection alive between calls and
// transparently re-establishes it. Single-threaded use per instance.
class HttpsClient {
public:
    explicit HttpsClient(RestClientOptions options);
    ~HttpsClient();
    HttpsClient(const HttpsClient&) = delete;
    HttpsClient& operator=(const HttpsClient&) = delete;

    // GET https://host{target}. Network-level failures are returned as errors;
    // HTTP responses (any status) are returned as HttpResponse.
    Result<HttpResponse> get(const std::string& target);

    // Generic request for authenticated exchange adapters.  Header values are
    // never logged.  Callers must pass an origin-form target beginning with
    // '/'; TLS hostname verification remains mandatory.
    Result<HttpResponse> request(const std::string& method, const std::string& target,
                                 std::string body,
                                 const std::vector<std::pair<std::string, std::string>>& headers = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Rate-limited GET with bounded retries + exponential backoff + jitter.
// `should_stop` aborts waiting during shutdown. Retries on network errors,
// timeouts, HTTP 429 and 5xx; other statuses are returned to the caller.
Result<HttpResponse> get_with_retry(HttpsClient& client, TokenBucket& bucket,
                                    const std::string& target, int32_t max_retries,
                                    int32_t backoff_min_ms, int32_t backoff_max_ms,
                                    const std::function<bool()>& should_stop);

} // namespace altspotter::exchanges
