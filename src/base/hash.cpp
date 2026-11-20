#include "base/hash.h"

#include <stdexcept>

#include <openssl/evp.h>

namespace fv {

namespace detail {

struct DigestContext {
    EVP_MD_CTX* ctx = nullptr;
    ~DigestContext() { EVP_MD_CTX_free(ctx); }
};

} // namespace detail

namespace {

[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string("SHA-256 failed in OpenSSL: ") + what);
}

std::string one_shot(const EVP_MD* md, std::string_view bytes) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), out, &size, md, nullptr) != 1) {
        throw std::runtime_error("Hashing failed in OpenSSL.");
    }
    return to_hex(std::span<const std::uint8_t>(out, size));
}

} // namespace

Sha256::Sha256() : context_(std::make_unique<detail::DigestContext>()) {
    context_->ctx = EVP_MD_CTX_new();
    if (context_->ctx == nullptr) {
        fail("EVP_MD_CTX_new");
    }
    if (EVP_DigestInit_ex(context_->ctx, EVP_sha256(), nullptr) != 1) {
        fail("EVP_DigestInit_ex");
    }
}

Sha256::~Sha256() = default;
Sha256::Sha256(Sha256&&) noexcept = default;
Sha256& Sha256::operator=(Sha256&&) noexcept = default;

void Sha256::update(std::string_view bytes) {
    if (EVP_DigestUpdate(context_->ctx, bytes.data(), bytes.size()) != 1) {
        fail("EVP_DigestUpdate");
    }
}

void Sha256::update(std::span<const std::byte> bytes) {
    if (EVP_DigestUpdate(context_->ctx, bytes.data(), bytes.size()) != 1) {
        fail("EVP_DigestUpdate");
    }
}

std::array<std::uint8_t, 32> Sha256::digest() const {
    // Finish a copy, so the running hash can be fed further.
    detail::DigestContext copy;
    copy.ctx = EVP_MD_CTX_new();
    if (copy.ctx == nullptr || EVP_MD_CTX_copy_ex(copy.ctx, context_->ctx) != 1) {
        fail("EVP_MD_CTX_copy_ex");
    }
    std::array<std::uint8_t, 32> out{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(copy.ctx, out.data(), &size) != 1 || size != out.size()) {
        fail("EVP_DigestFinal_ex");
    }
    return out;
}

std::string Sha256::hexdigest() const {
    const auto bytes = digest();
    return to_hex(bytes);
}

std::string sha256_hex(std::string_view bytes) {
    return one_shot(EVP_sha256(), bytes);
}

std::string sha1_hex(std::string_view bytes) {
    return one_shot(EVP_sha1(), bytes);
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 0x0f]);
    }
    return text;
}

} // namespace fv
