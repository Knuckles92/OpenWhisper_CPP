// verify_endpoint_key's KeyCheckOptions: create_openai_client's `api_key or resolve_api_key`,
// the SDK's OPENAI_BASE_URL and its HTTP-date Retry-After.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/testing/map_env_reader.h"
#include "cloud_test_support.h"
#include "speech/endpoint_key_check.h"

using namespace fv;
using namespace fv::speech;
using namespace fv::speech::test;

TEST_CASE("an empty key is tried as the stored one", "[endpoint_key_check]") {
    http::FakeHttpClient http;
    http.push(json_reply(200, "{}"));
    MapEnvReader env;
    const auto openai = configured_endpoint("openai", Json::object());
    KeyCheckOptions options;
    options.env = &env;
    options.stored_key = [] {
        return std::optional<std::string>("sk-saved");
    };
    CHECK(verify_endpoint_key(http, openai, "", {}, options).detail ==
          "api.openai.com accepted the key.");
    REQUIRE(http.requests().size() == 1);
    CHECK(http::find_header(http.requests()[0].headers, "Authorization") == "Bearer sk-saved");

    // A typed key wins over the stored one.
    http.push(json_reply(200, "{}"));
    verify_endpoint_key(http, openai, "sk-typed", {}, options);
    CHECK(http::find_header(http.requests()[1].headers, "Authorization") == "Bearer sk-typed");

    // Nothing stored either: the RuntimeError create_openai_client raises, before any request.
    options.stored_key = [] {
        return std::optional<std::string>();
    };
    CHECK(verify_endpoint_key(http, openai, "", {}, options).detail ==
          "Verification failed (RuntimeError).");
    options.stored_key = [] {
        return std::optional<std::string>("");
    };
    CHECK(verify_endpoint_key(http, openai, "", {}, options).detail ==
          "Verification failed (RuntimeError).");
    CHECK(http.requests().size() == 2);
}

TEST_CASE("OpenAI's request follows OPENAI_BASE_URL and is still named api.openai.com",
          "[endpoint_key_check]") {
    http::FakeHttpClient http;
    http.push(json_reply(200, "{}"));
    MapEnvReader env{{"OPENAI_BASE_URL", "https://proxy.example.test/v1/"}};
    KeyCheckOptions options;
    options.env = &env;
    const auto openai = configured_endpoint("openai", Json::object());
    CHECK(verify_endpoint_key(http, openai, "sk-test", {}, options).detail ==
          "api.openai.com accepted the key.");
    REQUIRE(http.requests().size() == 1);
    CHECK(http.requests()[0].url == "https://proxy.example.test/v1/models");

    // A server with its own address ignores the variable.
    http.push(json_reply(200, "{}"));
    Json settings = Json::object();
    save_server(settings, "custom", "http://127.0.0.1:9/v1", "CUSTOM_KEY", "m");
    verify_endpoint_key(http, configured_endpoint("custom", settings), "sk-test", {}, options);
    CHECK(http.requests()[1].url == "http://127.0.0.1:9/v1/models");
}

TEST_CASE("an HTTP-date Retry-After is waited for when it is within a minute",
          "[endpoint_key_check]") {
    using namespace std::chrono;
    // Wed, 21 Oct 2026 07:28:00 GMT, and the server asks for 07:28:05.
    const auto now = sys_days{2026y / October / 21} + 7h + 28min;
    FakeClock clock(now);
    http::FakeHttpClient http;
    http::ScriptedResponse limited;
    limited.status = 429;
    limited.headers = {{"retry-after", "Wed, 21 Oct 2026 07:28:05 GMT"}};
    http.push(limited);
    limited.headers = {{"retry-after", "Wed, 21 Oct 2026 08:28:00 GMT"}}; // an hour: backoff
    http.push(limited);
    http.push(json_reply(200, "{}"));
    std::vector<milliseconds> waits;
    KeyCheckOptions options;
    options.clock = &clock;
    options.wait = [&waits](milliseconds delay, const CancelToken&) {
        waits.push_back(delay);
    };
    const auto endpoint = configured_endpoint("speaches", Json::object());
    CHECK(verify_endpoint_key(http, endpoint, "", {}, options).ok);
    REQUIRE(waits.size() == 2);
    CHECK(waits[0] == milliseconds(5000));
    // The SDK's own backoff for the second retry: 1 s less up to a quarter.
    CHECK(waits[1] >= milliseconds(750));
    CHECK(waits[1] <= milliseconds(1000));
}
