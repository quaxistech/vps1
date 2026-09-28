#pragma once

#include "altmacro/core/error.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace altmacro::core {

struct HttpResponse {
    int status = 0;
    std::string body;      // may be binary (Dukascopy ships LZMA)
    std::map<std::string, std::string> headers;
};

// The User-Agent every provider sends.
//
// This is not cosmetic. FRED's edge refuses a bare product token outright --
// `altmacro/1.0` is answered with a reset connection, reproducibly, 0 of 3 --
// and so is a Mozilla impersonation. What it accepts is the conventional
// contact form `product/version (+URL)`, or a well-known client token such as
// curl or wget.
//
// Claiming to be curl would be a lie about what is making the request, so the
// default identifies this software truthfully and uses the IANA-reserved
// `.invalid` TLD, which is the conventional way to say "no contact URL has
// been configured" without inventing a page that does not exist. An operator
// with a real contact address should set `user_agent` in the configuration.
[[nodiscard]] const std::string& default_user_agent();
void set_default_user_agent(std::string ua);

struct HttpOptions {
    std::string host;
    std::string port = "443";
    int32_t connect_timeout_ms = 8000;
    int32_t request_timeout_ms = 30000;
    // Empty means "use the configured default", resolved at request time so a
    // provider constructed before configuration is loaded still sends the
    // right thing.
    std::string user_agent;
};

// Synchronous HTTPS/1.1 client (Boost.Beast + OpenSSL, certificate
// verification enforced). Keeps the connection alive between calls and
// re-establishes it transparently. One instance per thread.
//
// Redirects are NOT followed silently: a cross-host redirect can change which
// organisation is answering, and this service records provenance per row, so a
// 3xx is returned to the caller to decide about.
class HttpsClient {
public:
    explicit HttpsClient(HttpOptions options);
    ~HttpsClient();
    HttpsClient(const HttpsClient&) = delete;
    HttpsClient& operator=(const HttpsClient&) = delete;

    [[nodiscard]] Result<HttpResponse> get(const std::string& target);

    [[nodiscard]] const std::string& host() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace altmacro::core
