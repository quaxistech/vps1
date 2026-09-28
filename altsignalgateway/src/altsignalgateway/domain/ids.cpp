#include "altsignalgateway/domain/ids.hpp"

#include "altsignalgateway/core/hash.hpp"

#include <openssl/rand.h>

#include <array>
#include <stdexcept>

namespace altsignalgateway::domain {

std::string derive_id(const char* prefix, const std::string& natural_key) {
    return std::string(prefix) + sha256_hex(std::string(prefix) + ":" + natural_key).substr(0, 32);
}

std::string make_message_id(const std::string& signal_id) {
    return derive_id("asgw_", signal_id);
}

std::string make_session_id() {
    std::array<unsigned char, 16> raw{};
    // A failure here means the CSPRNG is unavailable. Continuing with a weak
    // session id would silently weaken the replay protection that binds a
    // signature to one connection, so it is fatal rather than degraded.
    if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed while minting a session id");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string out = "sess_";
    out.reserve(out.size() + raw.size() * 2);
    for (unsigned char b : raw) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

} // namespace altsignalgateway::domain
