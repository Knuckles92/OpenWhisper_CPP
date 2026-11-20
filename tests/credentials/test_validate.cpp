// Key and name validation and the key mask, replayed from OpenWhisper's services/credentials.py.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stdexcept>
#include <string>

#include "credentials/store.h"
#include "credentials/validate.h"
#include "golden/golden.h"

namespace golden = fv::golden;

namespace {

// Python's None reaches these functions as `raw or ""`.
std::string text_arg(const golden::Case& c) {
    return c.arg<std::optional<std::string>>(0).value_or("");
}

} // namespace

TEST_CASE("golden validate_api_key", "[golden]") {
    golden::for_each("services.credentials", "validate_api_key", [](const golden::Case& c) {
        if (c.raises()) {
            CHECK(c.raises_type() == "ValueError");
            CHECK_THROWS_AS(fv::validate_api_key(text_arg(c)), std::invalid_argument);
        } else {
            CHECK(fv::validate_api_key(text_arg(c)) == c.result<std::string>());
        }
    });
}

TEST_CASE("golden validate_api_key copy", "[golden]") {
    golden::for_each("services.credentials", "validate_api_key_error", [](const golden::Case& c) {
        std::optional<std::string> message;
        try {
            fv::validate_api_key(text_arg(c));
        } catch (const std::invalid_argument& error) {
            message = error.what();
        }
        CHECK(message == c.result<std::optional<std::string>>());
    });
}

TEST_CASE("golden mask_key", "[golden]") {
    golden::for_each("services.credentials", "mask_key", [](const golden::Case& c) {
        CHECK(fv::mask_key(text_arg(c)) == c.result<std::string>());
    });
}

TEST_CASE("golden validate_credential_name", "[golden]") {
    golden::for_each("services.credentials", "validate_credential_name", [](const golden::Case& c) {
        if (c.raises()) {
            CHECK(c.raises_type() == "ValueError");
            CHECK_THROWS_AS(fv::validate_credential_name(text_arg(c)), std::invalid_argument);
        } else {
            CHECK(fv::validate_credential_name(text_arg(c)) == c.result<std::string>());
        }
    });
}

TEST_CASE("the unresponsive keyring copy is verbatim") {
    // credentials.py:38 and 440-448.
    CHECK(fv::kKeyringNoResponse == "the system keyring did not respond");
    CHECK(fv::kUnresponsiveKeyringStatus ==
          "The system keyring did not respond. Saved API keys are unavailable.");
    CHECK(fv::keyring_did_not_respond(
        {false, "Secret Service", "the system keyring did not respond"}));
    CHECK_FALSE(fv::keyring_did_not_respond({false, "Secret Service", "locked"}));
    CHECK(fv::kMaxApiKeyLength == 512);
    CHECK(fv::kMinMaskSuffixLength == 12);
}

TEST_CASE("an empty credential name says so") {
    try {
        fv::validate_credential_name(" \t");
        FAIL("an empty name was accepted");
    } catch (const std::invalid_argument& error) {
        CHECK(std::string(error.what()) == "Credential name is empty");
    }
}
