#pragma once
// Minimal JSON encoding for the few values AltTech stores in JSONB columns.
//
// SQL quoting and JSON quoting are different things: a SQL literal is
// single-quoted, JSON strings are double-quoted. Passing a SQL-quoted value into
// a JSONB parameter is rejected by PostgreSQL, so the encoding is explicit here
// rather than improvised at each call site.

#include <cstdio>
#include <string>
#include <vector>

namespace alttech::crossvenue {

[[nodiscard]] inline std::string json_string(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out += '"';
    for (const char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

[[nodiscard]] inline std::string json_string_array(const std::vector<std::string>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out += ',';
        out += json_string(values[i]);
    }
    out += ']';
    return out;
}

} // namespace alttech::crossvenue
