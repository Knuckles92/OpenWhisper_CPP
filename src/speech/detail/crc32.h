#pragma once

#include <cstdint>
#include <span>

namespace fv::speech::detail {

// zlib.crc32(data, value): the IEEE CRC-32 (reflected polynomial 0xEDB88320), continued from
// `value`, so crc32(b, crc32(a)) == crc32(a + b).
// Affinity: any thread; no shared state.
std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t value = 0);

} // namespace fv::speech::detail
