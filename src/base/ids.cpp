#include "base/ids.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include <openssl/rand.h>

#include "base/hash.h"

namespace fv {

void random_bytes(std::span<std::byte> out) {
    auto* data = reinterpret_cast<unsigned char*>(out.data());
    std::size_t left = out.size();
    while (left > 0) {
        const int chunk = static_cast<int>(
            std::min<std::size_t>(left, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        if (RAND_bytes(data, chunk) != 1) {
            throw std::runtime_error("The system's random number generator failed.");
        }
        data += chunk;
        left -= static_cast<std::size_t>(chunk);
    }
}

std::string random_hex(std::size_t chars) {
    std::vector<std::uint8_t> bytes((chars + 1) / 2);
    random_bytes(std::as_writable_bytes(std::span(bytes)));
    std::string text = to_hex(bytes);
    text.resize(chars);
    return text;
}

std::string token_urlsafe(std::size_t nbytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::vector<std::uint8_t> bytes(nbytes);
    random_bytes(std::as_writable_bytes(std::span(bytes)));
    std::string text;
    text.reserve((nbytes * 4 + 2) / 3);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t group =
            (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8) | bytes[i + 2];
        text.push_back(alphabet[(group >> 18) & 63]);
        text.push_back(alphabet[(group >> 12) & 63]);
        text.push_back(alphabet[(group >> 6) & 63]);
        text.push_back(alphabet[group & 63]);
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t group = std::uint32_t{bytes[i]} << 16;
        text.push_back(alphabet[(group >> 18) & 63]);
        text.push_back(alphabet[(group >> 12) & 63]);
    } else if (rest == 2) {
        const std::uint32_t group =
            (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8);
        text.push_back(alphabet[(group >> 18) & 63]);
        text.push_back(alphabet[(group >> 12) & 63]);
        text.push_back(alphabet[(group >> 6) & 63]);
    }
    return text;
}

std::string uuid4_hex() {
    std::array<std::uint8_t, 16> bytes{};
    random_bytes(std::as_writable_bytes(std::span(bytes)));
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
    return to_hex(bytes);
}

} // namespace fv
