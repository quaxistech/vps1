#pragma once

#include "altsignalquality/core/error.hpp"
#include "altsignalquality/core/hash.hpp"
#include <fstream>

namespace altsignalquality {

// Read once into an owned, bounded text buffer. Callers must load this buffer,
// never reopen the pathname after verifying it. Canonical root confinement
// remains the caller's responsibility.
inline Result<std::string> verified_model_bytes(const std::string& path,
                                                const std::string& checksum) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return fail(ErrorCode::validation_error, "model artifact unreadable");
    const auto length = input.tellg();
    constexpr std::streamoff max_bytes = 128 * 1024 * 1024;
    if (length <= 0 || length > max_bytes)
        return fail(ErrorCode::validation_error, "model artifact size invalid");
    std::string bytes(static_cast<size_t>(length), '\0');
    input.seekg(0);
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof() ||
        bytes.find('\0') != std::string::npos)
        return fail(ErrorCode::validation_error, "model artifact incomplete or contains NUL");
    if (sha256_hex(bytes) != checksum)
        return fail(ErrorCode::validation_error, "model artifact checksum mismatch");
    return bytes;
}

} // namespace altsignalquality
