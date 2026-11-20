#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <set>
#include <string>

#include "automation/signing.h"
#include "base/base64.h"
#include "pyport/errors.h"

// from tests/test_automation_foundation_signing.py

using namespace fv::automation::signing;
namespace py = fv::py;

namespace {

// The published Standard Webhooks example (standard-webhooks/standard-webhooks README).
const std::string kSecret = "whsec_MfKQ9r8GKYqrTwjUPD8ILPZIo2LaLaSw";
const std::string kMsgId = "msg_p5jXN8AQM9LWM0D4loKWxJek";
constexpr std::int64_t kTimestamp = 1614265330;
const std::string kBody = R"({"test": 2432232314})";
const std::string kExpected = "v1,g0hM9SsE+OTPJTGt/tmIKtSyZlE3uFJELVlNIOLJ1OE=";

Received as_received(const std::vector<std::pair<std::string, std::string>>& headers) {
    return Received(headers.begin(), headers.end());
}

} // namespace

TEST_CASE("test_matches_the_published_example", "[test_automation_foundation_signing]") {
    CHECK(signature(kSecret, kMsgId, kTimestamp, kBody) == kExpected);
}

TEST_CASE("test_headers_round_trip_through_verify", "[test_automation_foundation_signing]") {
    const std::string secret = new_secret();
    CHECK(secret.starts_with("whsec_"));
    const auto headers_ = headers(secret, "evt_1", "{}", 1000);
    std::set<std::string> names;
    for (const auto& [name, value] : headers_) {
        names.insert(name);
    }
    CHECK(names == std::set<std::string>{"webhook-id", "webhook-timestamp", "webhook-signature"});
    const Received received = as_received(headers_);
    CHECK(verify(secret, received, "{}", DEFAULT_TOLERANCE_S, 1000));
    CHECK_FALSE(verify(secret, received, "{ }", DEFAULT_TOLERANCE_S, 1000));
    CHECK_FALSE(verify(new_secret(), received, "{}", DEFAULT_TOLERANCE_S, 1000));
    CHECK_FALSE(verify(secret, received, "{}", DEFAULT_TOLERANCE_S, 1000 + 301));
}

TEST_CASE("test_verify_accepts_one_of_several_signatures", "[test_automation_foundation_signing]") {
    const Received received = {
        {"Webhook-Id", kMsgId},
        {"Webhook-Timestamp", std::to_string(kTimestamp)},
        {"Webhook-Signature", "v1,bogus " + kExpected},
    };
    CHECK(verify(kSecret, received, kBody, DEFAULT_TOLERANCE_S, kTimestamp));
}

TEST_CASE("test_bad_secrets_are_refused", "[test_automation_foundation_signing]") {
    const std::string secret = GENERATE(as<std::string>{}, "", "MfKQ9r8", "whsec_", "whsec_!!!");
    CAPTURE(secret);
    CHECK_THROWS_AS(signature(secret, "id", 1, ""), py::ValueError);
    const Received received = {
        {"webhook-id", "id"}, {"webhook-timestamp", "1"}, {"webhook-signature", "v1,x"}};
    CHECK_FALSE(verify(secret, received, "", DEFAULT_TOLERANCE_S, 1));
}

TEST_CASE("a new secret holds 24 random bytes", "[signing]") {
    const std::string secret = new_secret();
    const auto key = fv::b64decode(std::string_view(secret).substr(PREFIX.size()));
    REQUIRE(key.has_value());
    CHECK(key->size() == 24);
    CHECK(new_secret() != secret);
}

TEST_CASE("headers default to the current time", "[signing]") {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const auto signed_headers = headers(kSecret, "evt_1", "{}");
    REQUIRE(signed_headers.size() == 3);
    CHECK(signed_headers[0] == std::pair<std::string, std::string>{"webhook-id", "evt_1"});
    const auto timestamp = std::stoll(signed_headers[1].second);
    CHECK(timestamp >= now);
    CHECK(timestamp <= now + 5);
    CHECK(verify(kSecret, as_received(signed_headers), "{}"));
}

TEST_CASE("a signature with non-ASCII characters is refused as compare_digest does", "[signing]") {
    const Received received = {{"webhook-id", kMsgId},
                               {"webhook-timestamp", std::to_string(kTimestamp)},
                               {"webhook-signature", "v1,caf\xC3\xA9"}};
    CHECK_THROWS_AS(verify(kSecret, received, kBody, DEFAULT_TOLERANCE_S, kTimestamp),
                    py::TypeError);
}
