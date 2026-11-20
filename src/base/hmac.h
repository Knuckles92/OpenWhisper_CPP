#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fv {

// hmac.new(key, message, hashlib.sha256).digest(). OpenSSL stays out of this header.
std::array<std::uint8_t, 32> hmac_sha256(std::span<const std::byte> key, std::string_view message);

// hmac.compare_digest over bytes: whether a and b are equal, in a time that depends on their
// lengths only, never on where they first differ. Python refuses str arguments with non-ASCII
// characters (TypeError); a caller comparing text checks that first.
bool constant_time_equal(std::string_view a, std::string_view b);

} // namespace fv
