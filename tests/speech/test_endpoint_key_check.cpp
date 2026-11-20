// verify_endpoint_key: text_llm.verify_api_key's GET {base}/models branch. Only the status class
// is reported; a provider's error body (which can echo the key) never is.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

#include "../http/loopback_server.h"
#include "cloud_test_support.h"
#include "speech/endpoint_key_check.h"

using namespace fv;
using namespace fv::speech;
using namespace fv::speech::test;
using fv::testing::LoopbackServer;

namespace {

std::shared_ptr<http::Client> direct_client() {
    return http::make_curl_client(http::CurlClientOptions{[](std::string_view) {
        return std::optional<std::string>();
    }});
}

SpeechEndpoint custom_at(const std::string& url, const std::string& env = "CUSTOM_KEY") {
    Json settings = Json::object();
    save_server(settings, "custom", url, env, "m");
    return configured_endpoint("custom", settings);
}

// Records the waits instead of sleeping.
struct Waits {
    std::vector<std::chrono::milliseconds> seen;
    RetryWait wait() {
        return [this](std::chrono::milliseconds delay, const CancelToken&) {
            seen.push_back(delay);
        };
    }
};

LoopbackServer answering(int status, std::string body,
                         std::vector<std::pair<std::string, std::string>> headers = {}) {
    return LoopbackServer([status, body = std::move(body), headers = std::move(headers)](
                              const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
        auto all = headers;
        all.emplace_back("Content-Type", "application/json");
        reply.write(LoopbackServer::response(status, "Status", all, body));
    });
}

} // namespace

TEST_CASE("a key the server accepts", "[endpoint_key_check]") {
    auto server = answering(200, cloud_reply("openai_models.json"));
    const auto client = direct_client();
    Waits waits;
    const auto check =
        verify_endpoint_key(*client, custom_at(server.url("/v1")), "sk-good", {}, waits.wait());
    CHECK(check.ok);
    CHECK(check.detail == "127.0.0.1 accepted the key.");
    const auto requests = server.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].method == "GET");
    CHECK(requests[0].target == "/v1/models");
    CHECK(requests[0].header("Authorization") == "Bearer sk-good");
    CHECK(waits.seen.empty());
}

TEST_CASE("401, 403 and other statuses are named without the body", "[endpoint_key_check]") {
    const auto client = direct_client();
    Waits waits;
    {
        auto server = answering(401, cloud_reply("openai_invalid_api_key.json"));
        const auto check =
            verify_endpoint_key(*client, custom_at(server.url("/v1")), "sk-test", {}, waits.wait());
        CHECK_FALSE(check.ok);
        CHECK(check.detail == "127.0.0.1 rejected the key (HTTP 401).");
        // The body quoted the key back; the result never does.
        CHECK(check.detail.find("sk-test") == std::string::npos);
        CHECK(server.requests().size() == 1);
    }
    {
        auto server = answering(403, R"({"error": {"message": "sk-test may not"}})");
        const auto check =
            verify_endpoint_key(*client, custom_at(server.url("/v1")), "sk-test", {}, waits.wait());
        CHECK(check.detail == "127.0.0.1 accepted the key but denied access (HTTP 403).");
    }
    {
        auto server = answering(404, cloud_reply("not_found.json"));
        const auto check =
            verify_endpoint_key(*client, custom_at(server.url("/v1")), "sk-test", {}, waits.wait());
        CHECK(check.detail == "127.0.0.1 answered HTTP 404.");
        CHECK(server.requests().size() == 1);
    }
    CHECK(waits.seen.empty());
}

TEST_CASE("server errors and rate limits are tried three times, as the SDK does",
          "[endpoint_key_check]") {
    const auto client = direct_client();
    Waits waits;
    auto server = answering(500, R"({"error": "down"})", {{"retry-after-ms", "1"}});
    const auto check =
        verify_endpoint_key(*client, custom_at(server.url("/v1")), "sk-test", {}, waits.wait());
    CHECK(check.detail == "127.0.0.1 answered HTTP 500.");
    CHECK(server.requests().size() == 3);
    // The server asked for 1 ms between attempts.
    CHECK(waits.seen == std::vector<std::chrono::milliseconds>{std::chrono::milliseconds(1),
                                                               std::chrono::milliseconds(1)});

    http::FakeHttpClient http;
    for (int i = 0; i < 3; ++i) {
        http::ScriptedResponse limited;
        limited.status = 429;
        http.push(limited);
    }
    Waits backoff;
    const auto endpoint = custom_at("http://127.0.0.1:1/v1");
    CHECK(verify_endpoint_key(http, endpoint, "sk-test", {}, backoff.wait()).detail ==
          "127.0.0.1 answered HTTP 429.");
    // 0.5 s, then 1 s, less up to a quarter of jitter.
    REQUIRE(backoff.seen.size() == 2);
    CHECK(backoff.seen[0] >= std::chrono::milliseconds(375));
    CHECK(backoff.seen[0] <= std::chrono::milliseconds(500));
    CHECK(backoff.seen[1] >= std::chrono::milliseconds(750));
    CHECK(backoff.seen[1] <= std::chrono::milliseconds(1000));
}

TEST_CASE("an unreachable host is named", "[endpoint_key_check]") {
    std::string url;
    {
        auto closed = answering(200, "{}");
        url = closed.url("/v1");
    }
    const auto client = direct_client();
    Waits waits;
    const auto check = verify_endpoint_key(*client, custom_at(url), "sk-test", {}, waits.wait());
    CHECK_FALSE(check.ok);
    CHECK(check.detail == "Couldn't reach 127.0.0.1.");
    CHECK(waits.seen.size() == 2);
}

TEST_CASE("OpenAI is named by its API host, and a missing key fails before any request",
          "[endpoint_key_check]") {
    http::FakeHttpClient http;
    http.push(json_reply(200, cloud_reply("openai_models.json")));
    const auto openai = configured_endpoint("openai", Json::object());
    const auto check = verify_endpoint_key(http, openai, "sk-test", {});
    CHECK(check.detail == "api.openai.com accepted the key.");
    REQUIRE(http.requests().size() == 1);
    CHECK(http.requests()[0].url == "https://api.openai.com/v1/models");
    CHECK(http.requests()[0].connect_timeout == std::chrono::milliseconds(10'000));
    CHECK(http.requests()[0].idle_timeout == std::chrono::milliseconds(10'000));

    CHECK(verify_endpoint_key(http, openai, "", {}).detail ==
          "Verification failed (RuntimeError).");
    CHECK(http.requests().size() == 1);

    // A server that needs no key is asked with the placeholder.
    http.push(json_reply(200, "{}"));
    const auto speaches = configured_endpoint("speaches", Json::object());
    CHECK(verify_endpoint_key(http, speaches, "", {}).detail == "127.0.0.1 accepted the key.");
    CHECK(http::find_header(http.requests()[1].headers, "Authorization") == "Bearer dummy");
}
