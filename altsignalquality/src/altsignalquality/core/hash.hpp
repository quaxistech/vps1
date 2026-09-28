#pragma once

#include <string>
#include <string_view>

namespace altsignalquality {

// Hex-encoded SHA-256 (OpenSSL EVP).
std::string sha256_hex(std::string_view data);

} // namespace altsignalquality
