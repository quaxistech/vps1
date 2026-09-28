#pragma once
// Internal AltTech runtime API.

#include <string>
#include <string_view>

namespace alttech {

// Hex-encoded SHA-256 (OpenSSL EVP).
std::string sha256_hex(std::string_view data);

} // namespace alttech
