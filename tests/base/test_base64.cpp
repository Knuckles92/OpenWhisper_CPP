#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "base/base64.h"

// Vectors from Python 3.12's base64 module: b64encode, urlsafe_b64encode (with and without its
// padding) and b64decode(..., validate=True).

namespace {

std::optional<std::vector<int>> decoded(std::string_view text, bool urlsafe = false) {
    const auto bytes = fv::b64decode(text, urlsafe);
    if (!bytes) {
        return std::nullopt;
    }
    std::vector<int> values;
    for (const std::byte b : *bytes) {
        values.push_back(std::to_integer<int>(b));
    }
    return values;
}

} // namespace

TEST_CASE("b64encode writes Python's standard, URL-safe and unpadded forms", "[base64]") {
    const auto [input, standard, urlsafe, unpadded] =
        GENERATE(table<std::string, std::string, std::string, std::string>({
            {"", "", "", ""},
            {"f", "Zg==", "Zg==", "Zg"},
            {"fo", "Zm8=", "Zm8=", "Zm8"},
            {"foo", "Zm9v", "Zm9v", "Zm9v"},
            {"foob", "Zm9vYg==", "Zm9vYg==", "Zm9vYg"},
            {"fooba", "Zm9vYmE=", "Zm9vYmE=", "Zm9vYmE"},
            {"foobar", "Zm9vYmFy", "Zm9vYmFy", "Zm9vYmFy"},
            {std::string("\x00\x01\x02\x03\x04\x05\x06", 7),
             "AAECAwQFBg==", "AAECAwQFBg==", "AAECAwQFBg"},
            {"\xFB\xFF\xFE", "+//+", "-__-", "-__-"},
        }));
    CAPTURE(standard);
    CHECK(fv::b64encode(input) == standard);
    CHECK(fv::b64encode(input, true) == urlsafe);
    CHECK(fv::b64encode(input, true, false) == unpadded);
    CHECK(fv::b64encode(std::as_bytes(std::span(input.data(), input.size()))) == standard);
}

TEST_CASE("b64decode is as strict as validate=True", "[base64]") {
    CHECK(decoded("") == std::vector<int>{});
    CHECK(decoded("Zg==") == std::vector<int>{102});
    CHECK(decoded("Zm8=") == std::vector<int>{102, 111});
    CHECK(decoded("Zm9v") == std::vector<int>{102, 111, 111});
    CHECK(decoded("Zm9vYg==") == std::vector<int>{102, 111, 111, 98});
    // Trailing bits of a short quad are not checked.
    CHECK(decoded("Zh==") == std::vector<int>{102});
    CHECK(decoded("+/+/") == std::vector<int>{251, 255, 191});
    // binascii.Error in Python: padding, length, stray characters and whitespace.
    for (const char* bad : {"Zg", "Zg=", "Z", "Zm9v=", "=Zm9v", "Zm9v YmFy", "Zm9v\n",
                            "Zm9vYmFy====", "Zg==Zg==", "Zg=a", "Zm=v", "-_-_", "Zg==\n", "Zm9vY",
                            "Zm9vYm", "Zm9v\xC3\xA9"}) {
        CAPTURE(bad);
        CHECK_FALSE(decoded(bad).has_value());
    }
}

TEST_CASE("b64decode urlsafe reads both alphabets, as Python's translation does", "[base64]") {
    CHECK(decoded("-_-_", true) == std::vector<int>{251, 255, 191});
    CHECK(decoded("+/+/", true) == std::vector<int>{251, 255, 191});
    CHECK(decoded("-_==", true) == std::vector<int>{251});
    CHECK_FALSE(decoded("-_", true).has_value());
}

TEST_CASE("b64decode reverses b64encode for every byte value", "[base64]") {
    std::string all;
    for (int b = 0; b < 256; ++b) {
        all += static_cast<char>(b);
    }
    for (std::size_t length = 0; length <= all.size(); length += 37) {
        const std::string part = all.substr(0, length);
        for (const bool urlsafe : {false, true}) {
            const auto back = fv::b64decode(fv::b64encode(part, urlsafe), urlsafe);
            REQUIRE(back.has_value());
            CHECK(std::string(reinterpret_cast<const char*>(back->data()), back->size()) == part);
        }
    }
}
