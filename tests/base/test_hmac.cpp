#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>

#include "base/hash.h"
#include "base/hmac.h"

// Vectors from Python 3.12's hmac.new(key, message, hashlib.sha256).hexdigest() and the RFC 4231
// test case 2.

namespace {

std::string hmac_hex(std::string_view key, std::string_view message) {
    const auto digest = fv::hmac_sha256(std::as_bytes(std::span(key.data(), key.size())), message);
    return fv::to_hex(digest);
}

} // namespace

TEST_CASE("hmac_sha256 matches Python's hmac", "[hmac]") {
    CHECK(hmac_hex("key", "The quick brown fox jumps over the lazy dog") ==
          "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    CHECK(hmac_hex("", "") == "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
    // A key longer than the block is hashed first.
    CHECK(hmac_hex(std::string(100, 'k'), "msg") ==
          "bd56a1782c2830e8abc6ed866a57a1230661e650b84c62f7ee3accc5fa5af491");
    std::string key;
    for (int b = 0; b < 32; ++b) {
        key += static_cast<char>(b);
    }
    CHECK(hmac_hex(key, "caf\xC3\xA9") ==
          "08e9f6a160b038033c3c010c374fe6c22de459bbdbf6349f735f5db075924b99");
    // RFC 4231, test case 2.
    CHECK(hmac_hex("Jefe", "what do ya want for nothing?") ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("constant_time_equal is equality of the bytes", "[hmac]") {
    CHECK(fv::constant_time_equal("", ""));
    CHECK(fv::constant_time_equal("abc", "abc"));
    CHECK_FALSE(fv::constant_time_equal("abc", "abd"));
    CHECK_FALSE(fv::constant_time_equal("abc", "ab"));
    CHECK_FALSE(fv::constant_time_equal("", "a"));
    CHECK_FALSE(fv::constant_time_equal("a", ""));
    CHECK(fv::constant_time_equal(std::string("a\0b", 3), std::string("a\0b", 3)));
    CHECK_FALSE(fv::constant_time_equal(std::string("a\0b", 3), std::string("a\0c", 3)));
}
