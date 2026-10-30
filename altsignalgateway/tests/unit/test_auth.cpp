// The ASGW-AUTH-v1 handshake.
//
// The scheme's whole security argument is that a signature is bound to ONE
// connection and ONE moment. Each of those bindings gets its own test, because
// each of them is one deleted line away from an endpoint anyone can talk to.

#include "altsignalgateway/domain/auth.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <limits>

using namespace altsignalgateway::domain;

namespace {

constexpr uint32_t kProto = 1;

ClientRegistry make_registry(const std::string& secret_hex = std::string(64, 'a')) {
    std::vector<RawCredential> raw;
    raw.push_back(RawCredential{"vps2-executor", "k1", secret_hex,
                                {"receive", "ack", "resume"}, 1});
    raw.push_back(RawCredential{"observer", "k2", std::string(64, 'b'), {"receive"}, 2});
    auto reg = ClientRegistry::build(raw);
    REQUIRE(reg.has_value());
    return std::move(*reg);
}

AuthRequest good_request(const ClientRegistry& reg, int64_t now_ms) {
    AuthRequest r;
    r.protocol_version = kProto;
    r.client_id = "vps2-executor";
    r.key_id = "k1";
    r.session_id = "sess_0123456789abcdef";
    r.server_nonce = std::string(32, '\x11');
    r.echoed_nonce = r.server_nonce;
    r.client_nonce = std::string(16, '\x22');
    r.timestamp_ms = now_ms;
    const auto* c = reg.by_key_id("k1");
    REQUIRE(c != nullptr);
    r.signature = hmac_sha256(
        c->secret, auth_canonical_string(kProto, r.client_id, r.key_id, r.session_id,
                                         r.server_nonce, r.client_nonce, r.timestamp_ms));
    return r;
}

} // namespace

TEST_CASE("a correct signature authenticates") {
    const auto reg = make_registry();
    const int64_t now = 1'787'000'000'000;
    const AuthorisedClient* who = nullptr;
    CHECK(verify_auth(reg, good_request(reg, now), now, 60'000, &who) == AuthFailure::none);
    REQUIRE(who != nullptr);
    CHECK(who->client_id == "vps2-executor");
    CHECK(who->has(Capability::receive));
    CHECK(who->has(Capability::ack));
    CHECK(who->has(Capability::resume));
}

TEST_CASE("a signature is bound to the connection that issued the nonce") {
    const auto reg = make_registry();
    const int64_t now = 1'787'000'000'000;

    SECTION("a nonce from another connection is refused") {
        auto req = good_request(reg, now);
        // The client signed the nonce it was given, but the server is now
        // holding a different one — which is exactly what a replay onto a new
        // connection looks like.
        req.server_nonce = std::string(32, '\x99');
        CHECK(verify_auth(reg, req, now, 60'000, nullptr) == AuthFailure::nonce_mismatch);
    }
    SECTION("echoing a nonce the server never issued is refused") {
        auto req = good_request(reg, now);
        req.echoed_nonce = std::string(32, '\x00');
        CHECK(verify_auth(reg, req, now, 60'000, nullptr) == AuthFailure::nonce_mismatch);
    }
    SECTION("a signature made for a different session id does not verify") {
        auto req = good_request(reg, now);
        // Same nonce, same timestamp, same key — only the session differs, and
        // the session id is inside the signed string.
        req.session_id = "sess_ffffffffffffffff";
        CHECK(verify_auth(reg, req, now, 60'000, nullptr) == AuthFailure::bad_signature);
    }
    SECTION("a signature made with a different client nonce does not verify") {
        auto req = good_request(reg, now);
        req.client_nonce = std::string(16, '\x33');
        CHECK(verify_auth(reg, req, now, 60'000, nullptr) == AuthFailure::bad_signature);
    }
}

TEST_CASE("a stale signature is refused") {
    const auto reg = make_registry();
    const int64_t now = 1'787'000'000'000;
    auto req = good_request(reg, now - 120'000);
    CHECK(verify_auth(reg, req, now, 60'000, nullptr) == AuthFailure::timestamp_skew);
    // A timestamp from the future is equally suspect: skew is absolute.
    auto future = good_request(reg, now + 120'000);
    CHECK(verify_auth(reg, future, now, 60'000, nullptr) == AuthFailure::timestamp_skew);
    // Inside the window it verifies.
    auto fresh = good_request(reg, now - 59'000);
    CHECK(verify_auth(reg, fresh, now, 60'000, nullptr) == AuthFailure::none);
}

TEST_CASE("authentication timestamp extremes and inclusive skew boundaries") {
    const auto reg = make_registry();
    constexpr int64_t now = 1'787'000'000'000;
    for (const auto timestamp : {std::numeric_limits<int64_t>::min(),
                                std::numeric_limits<int64_t>::max(), int64_t{0}, int64_t{-1},
                                now - 60'001, now + 60'001}) {
        CHECK(verify_auth(reg, good_request(reg, timestamp), now, 60'000, nullptr) ==
              AuthFailure::timestamp_skew);
    }
    for (const auto timestamp : {now - 60'000, now, now + 60'000}) {
        CHECK(verify_auth(reg, good_request(reg, timestamp), now, 60'000, nullptr) ==
              AuthFailure::none);
    }
    CHECK(verify_auth(reg, good_request(reg, now), now, -1, nullptr) == AuthFailure::timestamp_skew);
}

TEST_CASE("identity cannot be borrowed") {
    const auto reg = make_registry();
    const int64_t now = 1'787'000'000'000;

    SECTION("an unknown key id") {
        auto req = good_request(reg, now);
        req.key_id = "k99";
        CHECK(verify_auth(reg, req, now, 60'000, nullptr) == AuthFailure::unknown_key_id);
    }
    SECTION("a valid key used under another client's name") {
        auto req = good_request(reg, now);
        req.client_id = "observer";
        CHECK(verify_auth(reg, req, now, 60'000, nullptr) == AuthFailure::client_id_mismatch);
    }
    SECTION("a wrong signature") {
        auto req = good_request(reg, now);
        req.signature[0] = static_cast<char>(req.signature[0] ^ 0x01);
        CHECK(verify_auth(reg, req, now, 60'000, nullptr) == AuthFailure::bad_signature);
    }
    SECTION("an empty signature") {
        auto req = good_request(reg, now);
        req.signature.clear();
        CHECK(verify_auth(reg, req, now, 60'000, nullptr) == AuthFailure::bad_signature);
    }
    SECTION("a signature for a different protocol version") {
        auto req = good_request(reg, now);
        req.protocol_version = 2;
        CHECK(verify_auth(reg, req, now, 60'000, nullptr) == AuthFailure::bad_signature);
    }
}

TEST_CASE("a credential without the receive capability cannot open a feed") {
    std::vector<RawCredential> raw;
    // A key that exists but grants nothing useful. Rejecting at AUTH rather
    // than at the first SIGNAL avoids a session that looks healthy and silently
    // never receives anything.
    raw.push_back(RawCredential{"acker", "k3", std::string(64, 'c'), {"ack"}, 1});
    auto reg = ClientRegistry::build(raw);
    REQUIRE(reg.has_value());

    AuthRequest req;
    req.protocol_version = kProto;
    req.client_id = "acker";
    req.key_id = "k3";
    req.session_id = "sess_1";
    req.server_nonce = std::string(32, '\x11');
    req.echoed_nonce = req.server_nonce;
    req.client_nonce = std::string(16, '\x22');
    req.timestamp_ms = 1'787'000'000'000;
    req.signature = hmac_sha256(
        reg->by_key_id("k3")->secret,
        auth_canonical_string(kProto, req.client_id, req.key_id, req.session_id, req.server_nonce,
                              req.client_nonce, req.timestamp_ms));
    CHECK(verify_auth(*reg, req, req.timestamp_ms, 60'000, nullptr) ==
          AuthFailure::missing_capability);
}

TEST_CASE("the canonical string is unambiguous") {
    // Two different field splits must never produce the same bytes to sign.
    // Without the separators, ("ab","c") and ("a","bc") would collide and a
    // signature for one identity would authenticate another.
    const auto a = auth_canonical_string(1, "ab", "c", "s", "n", "m", 5);
    const auto b = auth_canonical_string(1, "a", "bc", "s", "n", "m", 5);
    CHECK(a != b);

    const auto expected = std::string("ASGW-AUTH-v1\n1\nclient\nkey\nsession\n") +
                          hex_encode("N") + "\n" + hex_encode("C") + "\n1234";
    CHECK(auth_canonical_string(1, "client", "key", "session", "N", "C", 1234) == expected);
}

TEST_CASE("hex codec is exact") {
    const std::string raw("\x00\x01\xfe\xff", 4);
    CHECK(hex_encode(raw) == "0001feff");
    auto back = hex_decode("0001feff");
    REQUIRE(back.has_value());
    CHECK(*back == raw);

    CHECK_FALSE(hex_decode("abc").has_value());     // odd length
    CHECK_FALSE(hex_decode("zz").has_value());      // not hex
    CHECK_FALSE(hex_decode("00 11").has_value());   // no silent skipping
}

TEST_CASE("constant-time comparison behaves like equality") {
    CHECK(constant_time_equal("abc", "abc"));
    CHECK_FALSE(constant_time_equal("abc", "abd"));
    CHECK_FALSE(constant_time_equal("abc", "ab"));
    CHECK(constant_time_equal("", ""));
}

TEST_CASE("HMAC-SHA256 matches the published test vector") {
    // RFC 4231 test case 1: a wrong HMAC construction that is merely
    // self-consistent would let the server and the reference client agree with
    // each other while nothing else in the world could interoperate.
    auto key = hex_decode("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    REQUIRE(key.has_value());
    const auto mac = hmac_sha256(*key, "Hi There");
    CHECK(hex_encode(mac) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST_CASE("a credential registry rejects weak key material") {
    std::vector<RawCredential> raw;
    raw.push_back(RawCredential{"weak", "kw", std::string(32, 'a'), {"receive"}, 1});  // 16 bytes
    CHECK_FALSE(ClientRegistry::build(raw).has_value());

    raw.clear();
    raw.push_back(RawCredential{"bad-hex", "kb", "zzzz", {"receive"}, 1});
    CHECK_FALSE(ClientRegistry::build(raw).has_value());
}

TEST_CASE("capability names round-trip") {
    for (const auto c : {Capability::receive, Capability::ack, Capability::resume}) {
        auto parsed = parse_capability(capability_name(c));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == c);
    }
    CHECK_FALSE(parse_capability("admin").has_value());
    CHECK_FALSE(parse_capability("write").has_value());
}

TEST_CASE("nonces are unpredictable") {
    // Not a statistical test: just an assertion that the CSPRNG is actually
    // being called rather than a counter or a constant.
    const auto a = random_bytes(32);
    const auto b = random_bytes(32);
    CHECK(a.size() == 32);
    CHECK(a != b);
    CHECK(a != std::string(32, '\0'));
}
