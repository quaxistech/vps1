#pragma once

// SQL literal builders shared by every store that writes vectors.
//
// These were three byte-identical copies of append_bytea() and two of
// append_float_array(), one per store. Both encode a PostgreSQL literal by
// hand, which is exactly the kind of code where a copy that misses a fix stays
// wrong quietly: the hex escape and the %.9g precision are correctness, not
// formatting -- %.9g is the shortest form that round-trips a float, and the
// bytea escape must not depend on the connection's bytea_output setting.

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace alttech::persistence::sql {

// '{...}'::real[] -- %.9g round-trips every float exactly.
inline void append_float_array(std::string& sql, const std::vector<float>& v) {
    sql += "'{";
    char buf[32];
    for (size_t i = 0; i < v.size(); ++i) {
        if (i != 0) sql += ',';
        std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v[i]));
        sql += buf;
    }
    sql += "}'::real[]";
}

// '\x...'::bytea -- the explicit hex form, independent of bytea_output.
inline void append_bytea(std::string& sql, const std::vector<uint8_t>& bytes) {
    static constexpr char hex[] = "0123456789abcdef";
    sql += "'\\x";
    for (const uint8_t b : bytes) {
        sql += hex[b >> 4];
        sql += hex[b & 0x0F];
    }
    sql += "'::bytea";
}

} // namespace alttech::persistence::sql
