#pragma once

// BYTEA text-literal codec.
//
// The journal stores two protobuf byte strings per signal and both are hashed,
// so the round trip has to be EXACT: a single byte that changes between write
// and read invalidates the integrity hash and, with it, the only mechanical
// proof that what was delivered is what was decided.
//
// libpqxx 7.8 still exposes `binarystring` but has deprecated it, and the
// replacement API differs between point releases. An explicit hex literal is
// stable across all of them and is what the sibling services already use, so
// the encoding is one convention on this stack rather than three.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace altsignalgateway::pgbytea {

// Encode raw bytes as a PostgreSQL hex bytea literal body: "\xdeadbeef".
// The caller binds it as a parameter and casts with ::bytea.
[[nodiscard]] inline std::string hex_literal(std::string_view raw) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2 + 2);
    out += "\\x";
    for (unsigned char c : raw) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

// Decode what PostgreSQL returns for a bytea column in hex output format.
// A malformed literal throws rather than returning a truncated buffer: a
// silently short payload would fail the hash check far from its cause.
[[nodiscard]] inline std::string from_hex_literal(std::string_view text) {
    if (text.size() < 2 || text[0] != '\\' || text[1] != 'x') {
        // The escape format (bytea_output = 'escape') is not produced by any
        // supported PostgreSQL default and is not decoded here on purpose:
        // guessing at an unexpected encoding is how a payload gets mangled.
        throw std::invalid_argument("bytea is not in hex output format");
    }
    const std::string_view body = text.substr(2);
    if ((body.size() % 2) != 0) throw std::invalid_argument("bytea hex body has an odd length");
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::invalid_argument("bytea hex body contains a non-hex character");
    };
    std::string out;
    out.reserve(body.size() / 2);
    for (size_t i = 0; i < body.size(); i += 2) {
        out.push_back(static_cast<char>((nibble(body[i]) << 4) | nibble(body[i + 1])));
    }
    return out;
}

} // namespace altsignalgateway::pgbytea
