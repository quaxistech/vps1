#pragma once

// Immutable ids + natural keys (§45). Every entity id is DETERMINISTIC in its
// natural key: NATS redelivery or a restart that replays a producer step yields
// the same id, so a UNIQUE(natural_key) upsert is idempotent. The natural key
// itself encodes the identity dimensions from §45 (canonical_pair_id,
// strategy_id, strategy_version, parameter_set_id, context_revision, event_time,
// proposal version).

#include "altstrategy/common/hash.hpp"

#include <string>

namespace altstrategy::engine::ids {

// 16-hex-char (64-bit) fingerprint of a natural key, prefixed by entity kind.
[[nodiscard]] inline std::string make(const std::string& prefix, const std::string& natural_key) {
    return prefix + "_" + altstrategy::sha256_hex(natural_key).substr(0, 24);
}

[[nodiscard]] inline std::string join(std::initializer_list<std::string> parts) {
    std::string out;
    bool first = true;
    for (const auto& p : parts) {
        if (!first) out.push_back('|');
        out += p;
        first = false;
    }
    return out;
}

[[nodiscard]] inline std::string i64(int64_t v) { return std::to_string(v); }

} // namespace altstrategy::engine::ids
