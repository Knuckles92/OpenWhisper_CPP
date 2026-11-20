// SHA-256 against the NIST FIPS 180-2 vectors, SHA-1, and the random ids.

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "base/hash.h"
#include "base/ids.h"

TEST_CASE("SHA-256 of the NIST vectors") {
    CHECK(fv::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(fv::sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(fv::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // One million "a", fed in pieces of 1000 as a stream.
    fv::Sha256 million;
    const std::string piece(1000, 'a');
    for (int i = 0; i < 1000; ++i) {
        million.update(piece);
    }
    CHECK(million.hexdigest() ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("a SHA-256 digest can be read midway and the hash fed further") {
    fv::Sha256 hash;
    hash.update(std::string_view("ab"));
    CHECK(hash.hexdigest() == fv::sha256_hex("ab"));
    const std::byte c{'c'};
    hash.update(std::span<const std::byte>(&c, 1));
    CHECK(hash.hexdigest() == fv::sha256_hex("abc"));
    const auto bytes = hash.digest();
    CHECK(bytes[0] == 0xba);
    CHECK(bytes[31] == 0xad);

    fv::Sha256 moved = std::move(hash);
    CHECK(moved.hexdigest() == fv::sha256_hex("abc"));
}

TEST_CASE("SHA-1 of the NIST vector") {
    CHECK(fv::sha1_hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST_CASE("random hex has the length asked for, in lowercase hex digits") {
    for (const std::size_t length : {0u, 1u, 12u, 32u, 33u}) {
        const auto text = fv::random_hex(length);
        CHECK(text.size() == length);
        CHECK(text.find_first_not_of("0123456789abcdef") == std::string::npos);
    }
    CHECK(fv::random_hex(32) != fv::random_hex(32));
}

TEST_CASE("token_urlsafe is unpadded URL-safe base64 of n bytes") {
    CHECK(fv::token_urlsafe().size() == 43); // secrets.token_urlsafe(): 32 bytes
    const std::size_t expected[] = {0, 2, 3, 4, 6, 7, 8};
    for (std::size_t n = 0; n <= 6; ++n) {
        const auto token = fv::token_urlsafe(n);
        CHECK(token.size() == expected[n]);
        CHECK(token.find_first_not_of(
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") ==
              std::string::npos);
    }
}

TEST_CASE("uuid4_hex is 32 hex digits with the version and variant bits") {
    std::set<std::string> seen;
    for (int i = 0; i < 64; ++i) {
        const auto id = fv::uuid4_hex();
        REQUIRE(id.size() == 32);
        CHECK(id.find_first_not_of("0123456789abcdef") == std::string::npos);
        CHECK(id[12] == '4');
        CHECK(std::string("89ab").find(id[16]) != std::string::npos);
        seen.insert(id);
    }
    CHECK(seen.size() == 64);
}
