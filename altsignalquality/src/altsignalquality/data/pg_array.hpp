#pragma once

// PostgreSQL array/bytea text-literal codecs for the positional feature vector.
//
// The feature vector is stored as REAL[] and its validity mask as BYTEA. libpqxx
// hands both over as text literals, so the round trip has to be exact: a value
// that changes between write and read would silently retrain the model on
// different numbers than inference sees.
//
// Both directions use <charconv>, never printf/stof, for two reasons that are
// not stylistic:
//
//   * LOCALE. `snprintf("%.9g")` and `std::stof` both honour the process
//     locale. Under a locale that uses a decimal comma, the writer would emit
//     `1,5` — which PostgreSQL parses as a two-element array — and the reader
//     would stop at the comma. `to_chars`/`from_chars` are defined to be
//     locale-independent.
//   * THROWING. `std::stof` throws `out_of_range` on a subnormal float, i.e. on
//     a value the writer itself can legitimately produce. That turns a routine
//     dataset read into an exception on the training path. `from_chars` reports
//     failure in its return value and handles subnormals correctly.
//
// `to_chars` with `chars_format::general` and no precision emits the SHORTEST
// representation that round-trips exactly, which is both smaller and more
// exact than a fixed `%.9g`.

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <system_error>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace altsignalquality::pgarray {

// Parse "{1.5,-2,NULL,NaN}". NULL and NaN both become NaN: an absent feature
// and an explicitly-absent feature mean the same thing to LightGBM.
[[nodiscard]] inline std::vector<float> parse_float_array(std::string_view text) {
    std::vector<float> out;
    if (text.size() < 2 || text.front() != '{' || text.back() != '}') {
        throw std::invalid_argument("not a PostgreSQL array literal");
    }
    const std::string_view body = text.substr(1, text.size() - 2);
    if (body.empty()) return out;
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find(',', start);
        if (end == std::string_view::npos) end = body.size();
        const std::string_view tok = body.substr(start, end - start);
        if (tok == "NULL" || tok == "NaN" || tok == "\"NaN\"") {
            out.push_back(std::numeric_limits<float>::quiet_NaN());
        } else {
            float value = 0;
            const auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), value);
            if (ec != std::errc{} || ptr != tok.data() + tok.size()) {
                throw std::invalid_argument("array element is not a float: " + std::string(tok));
            }
            out.push_back(value);
        }
        if (end == body.size()) break;
        start = end + 1;
    }
    return out;
}

template <typename FloatRange>
[[nodiscard]] inline std::string float_array_literal(const FloatRange& values) {
    std::string out = "{";
    bool first = true;
    char buf[64];
    for (const float v : values) {
        if (!first) out += ',';
        first = false;
        if (std::isnan(v)) {
            out += "NaN";
            continue;
        }
        if (std::isinf(v)) {
            // An infinity in a feature vector is a computation that went wrong
            // upstream of here; writing it would hand the model a value no
            // split can be reasoned about. The assembler never produces one —
            // every ratio guards its denominator — so reaching this is a bug,
            // and it says so rather than persisting the value.
            throw std::invalid_argument("feature vector contains an infinity");
        }
        const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v,
                                             std::chars_format::general);
        if (ec != std::errc{}) throw std::invalid_argument("float could not be formatted");
        out.append(buf, static_cast<size_t>(ptr - buf));
    }
    out += '}';
    return out;
}

[[nodiscard]] inline std::vector<uint8_t> parse_hex_bytea(std::string_view text) {
    if (text.size() < 2 || text[0] != '\\' || (text[1] != 'x' && text[1] != 'X')) {
        throw std::invalid_argument("not a hex bytea literal");
    }
    const std::string_view hex = text.substr(2);
    if (hex.size() % 2 != 0) throw std::invalid_argument("odd-length bytea hex");
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::invalid_argument("invalid bytea hex digit");
    };
    std::vector<uint8_t> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<uint8_t>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
    }
    return out;
}

[[nodiscard]] inline std::string bytea_hex_literal(const std::vector<uint8_t>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "\\x";
    out.reserve(2 + bytes.size() * 2);
    for (const uint8_t b : bytes) {
        out += kHex[b >> 4];
        out += kHex[b & 0x0F];
    }
    return out;
}

// PostgreSQL text[] literal from a vector of strings (quoted + escaped).
[[nodiscard]] inline std::string text_array_literal(const std::vector<std::string>& v) {
    std::string s = "{";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i != 0) s += ',';
        s += '"';
        for (const char ch : v[i]) {
            if (ch == '"' || ch == '\\') s += '\\';
            s += ch;
        }
        s += '"';
    }
    s += '}';
    return s;
}

} // namespace altsignalquality::pgarray
