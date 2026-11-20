// services/text_llm.py's requests to a text-model endpoint: key verification and model lists,
// through http::FakeHttpClient and a loopback server. Nothing leaves 127.0.0.1.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../http/loopback_server.h"
#include "golden/golden.h"
#include "http/testing/fake_http_client.h"
#include "llm/catalog_client.h"
#include "llm/local_servers.h"
#include "llm/model_catalog.h"
#include "llm/profiles.h"
#include "llm_test_support.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;
using fv::testing::LoopbackServer;

namespace {

constexpr std::string_view kSecret = "sk-test-SECRET-0123456789";

RetryWait no_wait() {
    return [](std::chrono::milliseconds, const CancelToken&) {
    };
}

std::shared_ptr<http::Client> direct_client() {
    return http::make_curl_client(http::CurlClientOptions{[](std::string_view) {
        return std::optional<std::string>();
    }});
}

TextLLMProfile custom_at(const std::string& url, const std::string& env = "CUSTOM_KEY") {
    Json settings = Json::object();
    return upsert_custom_profile(settings, "Gateway", url, env);
}

http::ScriptedResponse models_reply(const std::vector<std::string>& ids) {
    Json data = Json::array();
    for (const auto& id : ids) {
        data.push_back({{"id", id}, {"object", "model"}});
    }
    return json_reply(200, Json{{"object", "list"}, {"data", data}});
}

// The case file's SCENARIOS, as a FakeHttpClient answers them.
http::ScriptedResponse scenario_reply(const std::string& scenario, const http::Request& request) {
    http::ScriptedResponse reply;
    if (request.url.starts_with("http:///")) {
        // No host: nothing can be reached.
        reply.error = http::Error{http::ErrorKind::Dns, "no host"};
        return reply;
    }
    if (scenario == "ok") {
        return models_reply({"z-model", "gpt-4o-mini", "whisper-1", "o4-mini", "a-model",
                             "gpt-4o-audio-preview", "dall-e-3", "llama-guard-4", "distil-whisper",
                             "llama-3.3-70b", "Compound-beta", "chatgpt-4o-latest",
                             "text-embedding-3-small"});
    }
    if (scenario == "ok_odd") {
        return json_reply(200, py::loads(R"({"data": [{"id": "b"}, {"x": 1}, "junk", {"id": 5},
                                                       {"id": "a"}]})"));
    }
    if (scenario == "ok_key") {
        return json_reply(200, py::loads(R"({"data": {"label": "sk-...", "limit": null}})"));
    }
    if (scenario == "401") {
        return json_reply(401,
                          Json{{"error",
                                {{"message", "Incorrect API key provided: " + std::string(kSecret)},
                                 {"type", "invalid_request_error"},
                                 {"code", "invalid_api_key"}}}});
    }
    if (scenario == "403") {
        return json_reply(
            403,
            py::loads(R"({"error": {"message": "Project does not have access", "code": null}})"));
    }
    if (scenario == "404") {
        return json_reply(404, Json{{"error", "not found"}});
    }
    if (scenario == "429") {
        return json_reply(429, Json{{"error", {{"message", "Rate limit"}, {"type", "requests"}}}});
    }
    if (scenario == "500") {
        return json_reply(500, Json{{"error", {{"message", "boom"}}}});
    }
    if (scenario == "502_text") {
        return text_reply(502, "upstream down\n");
    }
    if (scenario == "401_empty") {
        return text_reply(401, "");
    }
    if (scenario == "200_text") {
        return text_reply(200, "plain");
    }
    if (scenario == "connect") {
        reply.error = http::Error{http::ErrorKind::Connect, "refused"};
        return reply;
    }
    if (scenario == "timeout") {
        reply.error = http::Error{http::ErrorKind::Timeout, "timed out"};
        return reply;
    }
    FAIL("unknown scenario " << scenario);
    return reply;
}

// What the case file records of each request.
Json recorded(const http::Request& request) {
    const auto path_start = request.url.find('/', request.url.find("://") + 3);
    std::string path = request.url.substr(path_start);
    std::string query;
    if (const auto question = path.find('?'); question != std::string::npos) {
        query = path.substr(question + 1);
        path.resize(question);
    }
    auto header = [&](std::string_view name) -> Json {
        const auto value = http::find_header(request.headers, name);
        return value ? Json(*value) : Json(nullptr);
    };
    return Json{{"method", request.method},
                {"path", path},
                {"query", query},
                {"bearer", http::find_header(request.headers, "Authorization") ==
                               "Bearer " + std::string(kSecret)},
                {"accept", header("Accept")},
                {"x-title", header("X-Title")},
                {"x-opencode-session", header("x-opencode-session")},
                {"user-agent", header("User-Agent")}};
}

// The vector's recorded requests with OpenWhisper's product names as OpenWhisper_CPP's.
Json rebranded(Json requests) {
    for (auto& request : requests) {
        for (const auto* name : {"x-title", "user-agent"}) {
            if (request[name].is_string()) {
                request[name] = golden::rebrand(request[name].get<std::string>());
            }
        }
    }
    return requests;
}

LoopbackServer answering(int status, std::string body) {
    return LoopbackServer([status, body = std::move(body)](const LoopbackServer::Request&,
                                                           LoopbackServer::Reply& reply) {
        reply.write(LoopbackServer::response(status, "Status",
                                             {{"Content-Type", "application/json"}}, body));
    });
}

} // namespace

// TestCredentialsAndCatalog

TEST_CASE("auth free profile uses dummy key", "[test_text_llm]") {
    // from tests/test_text_llm.py::TestCredentialsAndCatalog::test_auth_free_profile_uses_dummy_key
    TestKeys keys;
    Json settings = Json::object();
    const auto profile = upsert_custom_profile(settings, "Local", "http://127.0.0.1:1234/v1");
    CHECK(resolve_api_key(profile, keys.sources) == "dummy");
    // The client is built with the placeholder and the profile's base URL.
    http::FakeHttpClient client;
    client.push(models_reply({}));
    CHECK(verify_api_key(client, profile, "", {}, &keys.sources, no_wait()).ok);
    CHECK(client.requests()[0].url == "http://127.0.0.1:1234/v1/models");
    CHECK(http::find_header(client.requests()[0].headers, "Authorization") == "Bearer dummy");
}

TEST_CASE("missing required key returns none", "[test_text_llm]") {
    // from
    // tests/test_text_llm.py::TestCredentialsAndCatalog::test_missing_required_key_returns_none
    TestKeys keys;
    CHECK_FALSE(resolve_api_key(*get_profile("openai", Json::object()), keys.sources));
}

TEST_CASE("openai catalog filters non chat models", "[test_text_llm]") {
    // from
    // tests/test_text_llm.py::TestCredentialsAndCatalog::test_openai_catalog_filters_non_chat_models
    CHECK(filter_openai_chat_models(
              {"gpt-4o-mini", "whisper-1", "gpt-4o-audio-preview", "o4-mini", "dall-e-3"}) ==
          std::vector<std::string>{"gpt-4o-mini", "o4-mini"});
}

TEST_CASE("list chat models sorts openrouter only", "[test_text_llm]") {
    // from
    // tests/test_text_llm.py::TestCredentialsAndCatalog::test_list_chat_models_sorts_openrouter_only
    const auto openai = *get_profile("openai", Json::object());
    const auto openrouter = *get_profile("openrouter", Json::object());
    const auto custom = custom_at("https://gateway.test/v1", "");
    {
        http::FakeHttpClient client;
        client.push(models_reply({"gpt-4o-mini", "whisper-1", "o4-mini"}));
        CHECK(list_chat_models(client, openai, kSecret, "", {}, nullptr, no_wait()) ==
              std::vector<std::string>{"gpt-4o-mini", "o4-mini"});
    }
    {
        http::FakeHttpClient client;
        client.push(models_reply({"z-model", "a-model"}));
        // The server's ranking is kept.
        CHECK(list_chat_models(client, openrouter, kSecret, "top-weekly", {}, nullptr, no_wait()) ==
              std::vector<std::string>{"z-model", "a-model"});
        CHECK(client.requests()[0].url == "https://openrouter.ai/api/v1/models?sort=top-weekly");
    }
    {
        http::FakeHttpClient client;
        client.push(models_reply({"zeta", "alpha"}));
        CHECK(list_chat_models(client, custom, "", "top-weekly", {}, nullptr, no_wait()) ==
              std::vector<std::string>{"alpha", "zeta"});
        CHECK(client.requests()[0].url == "https://gateway.test/v1/models");
    }
}

TEST_CASE("list chat models propagates catalog failure", "[test_text_llm]") {
    // from
    // tests/test_text_llm.py::TestCredentialsAndCatalog::test_list_chat_models_propagates_catalog_failure
    {
        http::FakeHttpClient client;
        client.set_handler([](const http::Request&) {
            http::ScriptedResponse reply;
            reply.error = http::Error{http::ErrorKind::Connect, "catalog down"};
            return reply;
        });
        CHECK_THROWS_AS(list_chat_models(client, custom_at("https://gateway.test/v1", ""), "", "",
                                         {}, nullptr, no_wait()),
                        CatalogError);
    }
    // Nothing answers on a loopback port that was just closed.
    std::string url;
    {
        auto server = answering(200, "{}");
        url = server.url("/v1");
    }
    forget_server_specs();
    const auto client = direct_client();
    Json settings = Json::object();
    const auto local = upsert_custom_profile(settings, "Local", url);
    try {
        list_chat_models(*client, local, "", "", {}, nullptr, no_wait());
        FAIL("expected a connection failure");
    } catch (const CatalogError& error) {
        CHECK(error.type_name() == "ConnectError");
    }
}

TEST_CASE("verify_api_key against a loopback server reports the status class only",
          "[catalog_client]") {
    const auto client = direct_client();
    const std::string body =
        R"({"error": {"message": "Incorrect API key provided: )" + std::string(kSecret) + R"("}})";
    struct Expect {
        int status;
        bool ok;
        std::string detail;
        std::size_t attempts;
    };
    for (const auto& expect : std::vector<Expect>{
             {200, true, "127.0.0.1 accepted the key.", 1},
             {401, false, "127.0.0.1 rejected the key (HTTP 401).", 1},
             {403, false, "127.0.0.1 accepted the key but denied access (HTTP 403).", 1},
             {500, false, "127.0.0.1 answered HTTP 500.", 3},
         }) {
        LogScope log;
        auto server = answering(expect.status, expect.status == 200 ? R"({"data": []})" : body);
        const auto check =
            verify_api_key(*client, custom_at(server.url("/v1")), kSecret, {}, nullptr, no_wait());
        CHECK(check.ok == expect.ok);
        CHECK(check.detail == expect.detail);
        const auto requests = server.requests();
        CHECK(requests.size() == expect.attempts);
        REQUIRE_FALSE(requests.empty());
        CHECK(requests[0].target == "/v1/models");
        CHECK(requests[0].header("Authorization") == "Bearer " + std::string(kSecret));
        CHECK(check.detail.find(kSecret) == std::string::npos);
        CHECK(log.sink->all().find(kSecret) == std::string::npos);
        CHECK(log.sink->all().find("Incorrect API key") == std::string::npos);
    }
    // A refused connection, after the SDK's two retries.
    std::string url;
    {
        auto server = answering(200, "{}");
        url = server.url("/v1");
    }
    std::vector<std::chrono::milliseconds> waits;
    const auto check = verify_api_key(
        *client, custom_at(url), kSecret, {}, nullptr,
        [&](std::chrono::milliseconds delay, const CancelToken&) { waits.push_back(delay); });
    CHECK_FALSE(check.ok);
    CHECK(check.detail == "Couldn't reach 127.0.0.1.");
    CHECK(waits.size() == 2);
}

TEST_CASE("a cancelled verification stops without a status", "[catalog_client]") {
    CancelSource source;
    source.cancel();
    http::FakeHttpClient client;
    client.set_handler([](const http::Request&) { return models_reply({}); });
    const auto check = verify_api_key(client, *builtin_profile("openai"), kSecret, source.token(),
                                      nullptr, no_wait());
    CHECK_FALSE(check.ok);
    CHECK(check.detail == "Verification failed (Cancelled).");
}

TEST_CASE("catalog client replays services.text_llm", "[golden]") {
    const std::string module = "services.text_llm";
    golden::for_each(module, "filter_openai_chat_models", [](const golden::Case& c) {
        CHECK(Json(filter_openai_chat_models(c.arg<std::vector<std::string>>(0))) ==
              c.result<Json>());
    });
    golden::for_each(module, "verify_api_key", [](const golden::Case& c) {
        const auto scenario = c.arg<std::string>(2);
        http::FakeHttpClient client;
        client.set_handler([&scenario](const http::Request& request) {
            return scenario_reply(scenario, request);
        });
        const auto check = verify_api_key(client, profile_from(c.arg<Json>(0)),
                                          c.arg<std::string>(1), {}, nullptr, no_wait());
        const Json expected = c.result<Json>();
        CHECK(golden::equal(expected.at("result"), Json::array({check.ok, check.detail})));
        Json requests = Json::array();
        for (const auto& request : client.requests()) {
            requests.push_back(recorded(request));
        }
        CHECK(golden::equal(rebranded(expected.at("requests")), requests));
    });
    golden::for_each(module, "list_chat_models", [](const golden::Case& c) {
        const auto scenario = c.arg<std::string>(3);
        http::FakeHttpClient client;
        client.set_handler([&scenario](const http::Request& request) {
            return scenario_reply(scenario, request);
        });
        Json actual;
        try {
            actual = Json(list_chat_models(
                client, profile_from(c.arg<Json>(0)), c.arg<std::string>(1),
                c.arg<std::optional<std::string>>(2).value_or(""), {}, nullptr, no_wait()));
        } catch (const CatalogError& error) {
            actual = Json{{"raises", std::string(error.type_name())}, {"message", error.what()}};
        }
        const Json expected = c.result<Json>();
        CHECK(golden::equal(expected.at("result"), actual));
        Json requests = Json::array();
        for (const auto& request : client.requests()) {
            requests.push_back(recorded(request));
        }
        CHECK(golden::equal(rebranded(expected.at("requests")), requests));
    });
}
