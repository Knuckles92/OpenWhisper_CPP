#include "speech/detail/crc32.h"

#include <array>

namespace fv::speech::detail {

namespace {

constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[n] = c;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kTable = make_table();

} // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t value) {
    std::uint32_t c = value ^ 0xFFFFFFFFu;
    for (const std::uint8_t byte : data) {
        c = kTable[(c ^ byte) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

} // namespace fv::speech::detail
