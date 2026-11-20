#include "base/hmac.h"

#include <stdexcept>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace fv {

std::array<std::uint8_t, 32> hmac_sha256(std::span<const std::byte> key, std::string_view message) {
    std::array<std::uint8_t, 32> digest{};
    unsigned int size = 0;
    // HMAC() takes a null key for "no key"; an empty key must still mean the empty key.
    static const unsigned char kEmpty = 0;
    const void* key_data = key.empty() ? static_cast<const void*>(&kEmpty) : key.data();
    if (HMAC(EVP_sha256(), key_data, static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest.data(),
             &size) == nullptr ||
        size != digest.size()) {
        throw std::runtime_error("HMAC-SHA256 failed in OpenSSL.");
    }
    return digest;
}

bool constant_time_equal(std::string_view a, std::string_view b) {
    // compare_digest also takes as long for unequal lengths: compare b with itself, then answer
    // false.
    const std::string_view left = a.size() == b.size() ? a : b;
    const bool same = b.empty() || CRYPTO_memcmp(left.data(), b.data(), b.size()) == 0;
    return same && a.size() == b.size();
}

} // namespace fv
