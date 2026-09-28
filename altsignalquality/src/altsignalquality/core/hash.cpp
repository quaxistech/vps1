#include "altsignalquality/core/hash.hpp"

#include <openssl/evp.h>

#include <array>
#include <stdexcept>

namespace altsignalquality {

std::string sha256_hex(std::string_view data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) throw std::runtime_error("EVP_MD_CTX_new failed");
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, data.data(), data.size()) == 1 &&
              EVP_DigestFinal_ex(ctx, digest.data(), &len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) throw std::runtime_error("SHA-256 digest failed");

    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(hex[digest[i] >> 4]);
        out.push_back(hex[digest[i] & 0x0F]);
    }
    return out;
}

} // namespace altsignalquality
