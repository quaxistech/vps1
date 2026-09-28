#pragma once

#include "altsignalgateway/core/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace altsignalgateway::domain {

// ---------------------------------------------------------------------------
// The AltSignalGateway authentication scheme: ASGW-AUTH-v1.
//
// HMAC-SHA256 challenge-response over TLS. The scheme was chosen over the
// alternatives for reasons that matter to THIS boundary:
//
//   * a static bearer token would travel on every connection and be replayable
//     forever by anyone who ever saw one;
//   * mTLS alone identifies a certificate, not a protocol client, and rotating
//     a compromised key means re-issuing a certificate on a box that may be
//     unattended — it is supported here as an OPTIONAL extra layer, not as the
//     identity;
//   * a signed handshake binds the proof to one connection: the server issues
//     a single-use 32-byte nonce, the client signs it together with the session
//     id and its own nonce, and the signature is worthless anywhere else.
//
// The canonical string is newline-separated and every component is fixed-order,
// so two different field sets can never produce the same bytes to sign:
//
//   ASGW-AUTH-v1\n
//   <protocol_version>\n
//   <client_id>\n
//   <key_id>\n
//   <session_id>\n
//   <hex(server_nonce)>\n
//   <hex(client_nonce)>\n
//   <timestamp_ms>
//
// Replay protection is layered:
//   1. server_nonce is CSPRNG, 32 bytes, generated per connection and accepted
//      exactly once — a captured Auth frame cannot be replayed onto a new one;
//   2. session_id is random per connection and is inside the signed string;
//   3. timestamp_ms must be within max_clock_skew of the server clock, which
//      bounds how long a signature is even syntactically current;
//   4. the comparison is constant-time.
// ---------------------------------------------------------------------------

inline constexpr std::string_view kAuthScheme = "HMAC-SHA256";
inline constexpr std::string_view kAuthDomain = "ASGW-AUTH-v1";

// Build the exact byte string both sides sign. Shared by the server and by the
// reference client, so the two can never drift apart.
[[nodiscard]] std::string auth_canonical_string(uint32_t protocol_version,
                                                std::string_view client_id,
                                                std::string_view key_id,
                                                std::string_view session_id,
                                                std::string_view server_nonce_raw,
                                                std::string_view client_nonce_raw,
                                                int64_t timestamp_ms);

// Raw 32-byte HMAC-SHA256.
[[nodiscard]] std::string hmac_sha256(std::string_view key, std::string_view data);

// Constant-time equality. Used for every signature comparison; a byte-wise
// early exit leaks the position of the first mismatch to a patient attacker.
[[nodiscard]] bool constant_time_equal(std::string_view a, std::string_view b);

// Decode a hex string to raw bytes. Rejects odd lengths and non-hex digits
// rather than skipping them.
[[nodiscard]] Result<std::string> hex_decode(std::string_view hex);

[[nodiscard]] std::string hex_encode(std::string_view raw);

// CSPRNG bytes. Throws if the CSPRNG is unavailable: a degraded nonce would
// silently remove the replay protection above.
[[nodiscard]] std::string random_bytes(size_t n);

// ---------------------------------------------------------------------------
// Client registry
// ---------------------------------------------------------------------------

enum class Capability : uint8_t {
    receive = 1,
    ack = 2,
    resume = 3,
    query = 4,      // protocol 2: read the journal and the extended context
};

[[nodiscard]] std::string_view capability_name(Capability c);
[[nodiscard]] Result<Capability> parse_capability(std::string_view name);

struct AuthorisedClient {
    std::string client_id;
    std::string key_id;
    std::string secret;   // raw key bytes, decoded from the configured hex
    std::vector<Capability> capabilities;
    int32_t max_sessions = 1;

    [[nodiscard]] bool has(Capability c) const;
};

// The set of identities the endpoint will accept, keyed by key_id.
//
// Secrets live only here and are never logged, never echoed in an error and
// never written to the journal. An authentication failure reports one opaque
// reason to the client (ERR_AUTH_FAILED) while the server-side log records
// which check failed — telling a caller *why* its signature was rejected is a
// free oracle.
class ClientRegistry {
public:
    static Result<ClientRegistry> build(const std::vector<struct RawCredential>& raw);

    [[nodiscard]] const AuthorisedClient* by_key_id(std::string_view key_id) const;
    [[nodiscard]] const AuthorisedClient* by_client_id(std::string_view client_id) const;
    [[nodiscard]] size_t size() const { return clients_.size(); }
    [[nodiscard]] const std::vector<AuthorisedClient>& all() const { return clients_; }

private:
    std::vector<AuthorisedClient> clients_;
};

// A credential as it appears in the configuration file, before decoding.
struct RawCredential {
    std::string client_id;
    std::string key_id;
    std::string secret_hex;
    std::vector<std::string> capabilities;
    int32_t max_sessions = 1;
};

// Why an authentication attempt failed. Reported to the operator, never to the
// client.
enum class AuthFailure {
    none = 0,
    unknown_key_id,
    client_id_mismatch,
    nonce_mismatch,
    timestamp_skew,
    bad_signature,
    missing_capability,
};

[[nodiscard]] std::string_view auth_failure_name(AuthFailure f);

struct AuthRequest {
    uint32_t protocol_version = 0;
    std::string client_id;         // from HELLO
    std::string key_id;            // from AUTH
    std::string session_id;        // server-issued
    std::string server_nonce;      // server-issued, raw
    std::string echoed_nonce;      // as returned by the client, raw
    std::string client_nonce;      // from HELLO, raw
    int64_t timestamp_ms = 0;
    std::string signature;         // raw 32 bytes
};

// Verify one AUTH frame. `now_ms` is passed in rather than read here so the
// skew logic is testable without a clock.
[[nodiscard]] AuthFailure verify_auth(const ClientRegistry& registry, const AuthRequest& req,
                                      int64_t now_ms, int64_t max_skew_ms,
                                      const AuthorisedClient** out_client);

} // namespace altsignalgateway::domain
