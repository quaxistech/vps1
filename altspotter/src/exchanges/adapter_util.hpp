#pragma once

// Helpers shared by every venue adapter.
//
// These existed as byte-identical copies in four adapter translation units
// (`opt_ll` x4, `host_from_url` x3). Four copies of one operation is four
// places to fix a parsing bug in, and three of them will be missed.

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <exception>
#include <string>

namespace altspotter::exchanges {

// Read an integer field that a venue may send as a JSON number OR as a string —
// most of them are inconsistent about it, sometimes within one payload.
//
// ABSENT, UNPARSEABLE AND ZERO ARE THE SAME VALUE HERE. That is deliberate and
// every caller depends on it: the fields read through this helper are optional
// venue metadata (`onboardDate`, `fundingTime`, `deliveryTime`) where 0 already
// means "not applicable", so a caller cannot act on the distinction. Do NOT use
// it for a field whose absence must be detected — a market-data timestamp, a
// sequence number, a price. Those parse explicitly and fail loudly.
[[nodiscard]] inline int64_t opt_ll(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end()) return 0;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_string()) {
        try {
            return std::stoll(it->get_ref<const std::string&>());
        } catch (const std::exception&) {
            return 0;
        }
    }
    return 0;
}

// Host part of a URL, for the TLS SNI and the HTTP Host header.
// `wss://stream.example.com/v5/public` -> `stream.example.com`.
[[nodiscard]] inline std::string host_from_url(const std::string& url) {
    const auto pos = url.find("://");
    std::string host = pos == std::string::npos ? url : url.substr(pos + 3);
    if (const auto slash = host.find('/'); slash != std::string::npos) {
        host = host.substr(0, slash);
    }
    return host;
}

// Parse a venue-supplied decimal string into a double.
//
// NON-FINITE INPUT IS NOT AN ERROR TO std::stod: it parses "nan" and "inf"
// happily and returns exactly those values. That matters wherever the result is
// then validated by comparison, because EVERY comparison against NaN is false —
// so a guard like `bid <= 0 || ask <= 0 || ask < bid` lets a NaN straight
// through, PostgreSQL accepts NaN in a `double precision` column, and the bad
// value propagates into the microstructure vectors and from there into the
// spread a trade is priced against.
//
// Non-finite and unparseable both yield 0.0, which the callers' `> 0` checks
// already reject.
[[nodiscard]] inline double to_double(const std::string& s) {
    try {
        const double v = std::stod(s);
        return std::isfinite(v) ? v : 0.0;
    } catch (const std::exception&) {
        return 0.0;
    }
}

} // namespace altspotter::exchanges
