#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace fv {

namespace detail {
struct DigestContext;
} // namespace detail

// Streaming SHA-256, as Python's hashlib.sha256: digest() may be read at any point and the hash
// fed further afterwards. OpenSSL stays out of this header. Move-only.
// Affinity: one thread at a time.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(Sha256&&) noexcept;
    Sha256& operator=(Sha256&&) noexcept;
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(std::string_view bytes);
    void update(std::span<const std::byte> bytes);
    std::array<std::uint8_t, 32> digest() const;
    std::string hexdigest() const; // 64 lowercase hex digits

private:
    std::unique_ptr<detail::DigestContext> context_;
};

std::string sha256_hex(std::string_view bytes);
std::string sha1_hex(std::string_view bytes);

// Lowercase hex of the bytes, two digits each.
std::string to_hex(std::span<const std::uint8_t> bytes);

} // namespace fv
