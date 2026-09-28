#include "altsignalgateway/domain/auth.hpp"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <stdexcept>

namespace altsignalgateway::domain {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::string hex_encode(std::string_view raw) {
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        out.push_back(kHexDigits[c >> 4]);
        out.push_back(kHexDigits[c & 0x0F]);
    }
    return out;
}

Result<std::string> hex_decode(std::string_view hex) {
    if ((hex.size() % 2) != 0) {
        return fail(ErrorCode::invalid_argument, "hex string has an odd length");
    }
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_value(static_cast<unsigned char>(hex[i]));
        const int lo = hex_value(static_cast<unsigned char>(hex[i + 1]));
        if (hi < 0 || lo < 0) {
            return fail(ErrorCode::invalid_argument, "hex string contains a non-hex character");
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

std::string random_bytes(size_t n) {
    std::string out(n, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(n)) != 1) {
        throw std::runtime_error("RAND_bytes failed: CSPRNG unavailable");
    }
    return out;
}

std::string auth_canonical_string(uint32_t protocol_version, std::string_view client_id,
                                  std::string_view key_id, std::string_view session_id,
                                  std::string_view server_nonce_raw,
                                  std::string_view client_nonce_raw, int64_t timestamp_ms) {
    std::string out;
    out.reserve(256);
    out.append(kAuthDomain);
    out.push_back('\n');
    out.append(std::to_string(protocol_version));
    out.push_back('\n');
    out.append(client_id);
    out.push_back('\n');
    out.append(key_id);
    out.push_back('\n');
    out.append(session_id);
    out.push_back('\n');
    out.append(hex_encode(server_nonce_raw));
    out.push_back('\n');
    out.append(hex_encode(client_nonce_raw));
    out.push_back('\n');
    out.append(std::to_string(timestamp_ms));
    return out;
}

std::string hmac_sha256(std::string_view key, std::string_view data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
    unsigned int len = 0;
    const unsigned char* r =
        ::HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
               reinterpret_cast<const unsigned char*>(data.data()), data.size(), mac.data(), &len);
    if (r == nullptr) throw std::runtime_error("HMAC-SHA256 failed");
    return std::string(reinterpret_cast<const char*>(mac.data()), len);
}

bool constant_time_equal(std::string_view a, std::string_view b) {
    // Length is not secret and comparing different lengths in constant time is
    // meaningless, but the early return must not be data-dependent beyond it.
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string_view capability_name(Capability c) {
    switch (c) {
        case Capability::receive: return "receive";
        case Capability::ack: return "ack";
        case Capability::resume: return "resume";
        case Capability::query: return "query";
    }
    return "unknown";
}

Result<Capability> parse_capability(std::string_view name) {
    if (name == "receive") return Capability::receive;
    if (name == "ack") return Capability::ack;
    if (name == "resume") return Capability::resume;
    if (name == "query") return Capability::query;
    return fail(ErrorCode::config_error, "unknown capability: " + std::string(name));
}

bool AuthorisedClient::has(Capability c) const {
    return std::find(capabilities.begin(), capabilities.end(), c) != capabilities.end();
}

Result<ClientRegistry> ClientRegistry::build(const std::vector<RawCredential>& raw) {
    ClientRegistry reg;
    reg.clients_.reserve(raw.size());
    for (const auto& r : raw) {
        AuthorisedClient c;
        c.client_id = r.client_id;
        c.key_id = r.key_id;
        auto secret = hex_decode(r.secret_hex);
        if (!secret) {
            return fail(ErrorCode::config_error,
                        "client '" + r.client_id + "': secret_hex is not valid hex");
        }
        c.secret = std::move(*secret);
        if (c.secret.size() < 32) {
            return fail(ErrorCode::config_error,
                        "client '" + r.client_id + "': key material shorter than 32 bytes");
        }
        for (const auto& name : r.capabilities) {
            auto cap = parse_capability(name);
            if (!cap) return std::unexpected(cap.error());
            c.capabilities.push_back(*cap);
        }
        c.max_sessions = r.max_sessions;
        reg.clients_.push_back(std::move(c));
    }
    return reg;
}

const AuthorisedClient* ClientRegistry::by_key_id(std::string_view key_id) const {
    for (const auto& c : clients_) {
        if (c.key_id == key_id) return &c;
    }
    return nullptr;
}

const AuthorisedClient* ClientRegistry::by_client_id(std::string_view client_id) const {
    for (const auto& c : clients_) {
        if (c.client_id == client_id) return &c;
    }
    return nullptr;
}

std::string_view auth_failure_name(AuthFailure f) {
    switch (f) {
        case AuthFailure::none: return "none";
        case AuthFailure::unknown_key_id: return "unknown_key_id";
        case AuthFailure::client_id_mismatch: return "client_id_mismatch";
        case AuthFailure::nonce_mismatch: return "nonce_mismatch";
        case AuthFailure::timestamp_skew: return "timestamp_skew";
        case AuthFailure::bad_signature: return "bad_signature";
        case AuthFailure::missing_capability: return "missing_capability";
    }
    return "unknown";
}

AuthFailure verify_auth(const ClientRegistry& registry, const AuthRequest& req, int64_t now_ms,
                        int64_t max_skew_ms, const AuthorisedClient** out_client) {
    if (out_client != nullptr) *out_client = nullptr;

    const AuthorisedClient* client = registry.by_key_id(req.key_id);
    if (client == nullptr) return AuthFailure::unknown_key_id;
    // The key names the identity; HELLO merely claims one. A mismatch means a
    // valid key is being used under someone else's name.
    if (client->client_id != req.client_id) return AuthFailure::client_id_mismatch;

    // The echoed nonce must be the one THIS connection issued. This is what
    // makes a captured signature useless on any other connection.
    if (!constant_time_equal(req.echoed_nonce, req.server_nonce)) {
        return AuthFailure::nonce_mismatch;
    }
    const int64_t skew = req.timestamp_ms > now_ms ? req.timestamp_ms - now_ms
                                                   : now_ms - req.timestamp_ms;
    if (skew > max_skew_ms) return AuthFailure::timestamp_skew;

    const std::string expected = hmac_sha256(
        client->secret,
        auth_canonical_string(req.protocol_version, req.client_id, req.key_id, req.session_id,
                              req.server_nonce, req.client_nonce, req.timestamp_ms));
    if (!constant_time_equal(expected, req.signature)) return AuthFailure::bad_signature;

    // A credential with no receive capability cannot use this endpoint at all;
    // rejecting at authentication rather than at the first SIGNAL avoids a
    // session that looks healthy and silently never gets anything.
    if (!client->has(Capability::receive)) return AuthFailure::missing_capability;

    if (out_client != nullptr) *out_client = client;
    return AuthFailure::none;
}

} // namespace altsignalgateway::domain
