// Sweep A24: verify_api_key and list_chat_models (detail::sdk_send) wait as the OpenAI SDK
// (3.6.0) and OpenAiCompatClient do: a retry-after of up to 120 s, seconds or an HTTP date, and
// no retry at all when the server asks for longer. Nothing leaves the process (FakeHttpClient).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "http/testing/fake_http_client.h"
#include "llm/catalog_client.h"
#include "llm/detail/sdk_request.h"
#include "llm/profiles.h"
#include "llm_test_support.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view kSecret = "sk-test-SECRET-0123456789";

http::ScriptedResponse busy(http::Headers headers) {
    auto reply = json_reply(429, Json{{"error", {{"message", "slow down"}}}});
    for (auto& header : headers) {
        reply.headers.push_back(std::move(header));
    }
    return reply;
}

http::ScriptedResponse models() {
    return json_reply(200,
                      Json{{"object", "list"}, {"data", Json::array({Json{{"id", "gpt-4o"}}})}});
}

struct Waits {
    std::vector<std::chrono::milliseconds> seen;
    RetryWait wait() {
        return [this](std::chrono::milliseconds delay, const CancelToken&) {
            seen.push_back(delay);
        };
    }
};

} // namespace

TEST_CASE("sdk_send waits a retry-after of up to 120 seconds", "[sweep][catalog_client]") {
    http::FakeHttpClient client;
    client.push(busy({{"retry-after", "90"}}));
    client.push(models());
    Waits waits;
    const auto check =
        verify_api_key(client, *builtin_profile("openai"), kSecret, {}, nullptr, waits.wait());
    CHECK(check.ok);
    REQUIRE(waits.seen.size() == 1);
    CHECK(waits.seen[0] == 90'000ms);
}

TEST_CASE("sdk_send reads a retry-after HTTP date from its clock", "[sweep][catalog_client]") {
    // 2026-01-01T00:00:00Z, the fake clock's wall time; the server asks for 100 s.
    FakeClock clock;
    http::FakeHttpClient client;
    client.push(busy({{"retry-after", "Thu, 01 Jan 2026 00:01:40 GMT"}}));
    client.push(models());
    http::Request request;
    request.url = "https://api.openai.com/v1/models";
    Waits waits;
    const auto response = llm::detail::sdk_send(client, request, {}, waits.wait(), clock);
    REQUIRE(response);
    CHECK(response->status == 200);
    REQUIRE(waits.seen.size() == 1);
    CHECK(waits.seen[0] == 100'000ms);
}

TEST_CASE("sdk_send does not retry when the server asks for more than 120 seconds",
          "[sweep][catalog_client]") {
    http::FakeHttpClient client;
    client.set_handler([](const http::Request&) { return busy({{"retry-after", "121"}}); });
    Waits waits;
    const auto profile = *builtin_profile("openai");
    try {
        list_chat_models(client, profile, kSecret, "", {}, nullptr, waits.wait());
        FAIL("a 429 answer is an error");
    } catch (const CatalogError& error) {
        CHECK(std::string(error.type_name()) == "RateLimitError");
    }
    CHECK(waits.seen.empty());
    CHECK(client.requests().size() == 1);
}
