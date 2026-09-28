#pragma once

#include <string>
#include <string_view>

namespace altml {

// Hex-encoded SHA-256 (OpenSSL EVP).
std::string sha256_hex(std::string_view data);

} // namespace altml
