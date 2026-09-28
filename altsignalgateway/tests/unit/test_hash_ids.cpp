// Hashing and identity derivation.
//
// Every id this service mints has to survive a crash: a redelivered signal must
// reproduce the SAME message id, or a consumer de-duplicating on it would see
// two distinct messages carrying one trade.

#include "altsignalgateway/core/hash.hpp"
#include "altsignalgateway/domain/ids.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using namespace altsignalgateway;

TEST_CASE("sha256 matches the published vectors") {
    // A hash that is merely self-consistent would let this service and its own
    // client agree while nothing else could verify a payload.
    CHECK(sha256_hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(std::string("\x00\x01\x02", 3)) ==
          "ae4b3280e56e2faf83f414a6e3dabe9d5fbe18976544c05fed121accb85b53fc");
}

TEST_CASE("a message id is deterministic in the signal it carries") {
    const std::string signal = "sqfin_0123456789abcdef0123456789abcdef";
    const auto a = domain::make_message_id(signal);
    const auto b = domain::make_message_id(signal);
    CHECK(a == b);
    // Prefix + 32 hex characters, the convention the rest of the stack uses.
    CHECK(a.rfind("asgw_", 0) == 0);
    CHECK(a.size() == 5 + 32);
    CHECK(a.find_first_not_of("0123456789abcdef", 5) == std::string::npos);
}

TEST_CASE("different signals get different message ids") {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        seen.insert(domain::make_message_id("sqfin_" + std::to_string(i)));
    }
    CHECK(seen.size() == 1000);
}

TEST_CASE("the id prefix is part of the hashed input") {
    // Otherwise two id families derived from the same natural key would collide
    // in everything but their prefix, and a truncated log line could not tell
    // them apart.
    CHECK(domain::derive_id("asgw_", "x").substr(5) != domain::derive_id("sqev_", "x").substr(5));
}

TEST_CASE("session ids are random, not derived") {
    // A predictable session id would let a captured signature be replayed onto
    // a predicted session, which is the one thing the nonce binding prevents.
    std::set<std::string> seen;
    for (int i = 0; i < 500; ++i) {
        const auto id = domain::make_session_id();
        CHECK(id.rfind("sess_", 0) == 0);
        CHECK(id.size() == 5 + 32);
        seen.insert(id);
    }
    CHECK(seen.size() == 500);
}
