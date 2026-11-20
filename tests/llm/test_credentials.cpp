// services/text_llm.py's credential half: key status copy, key resolution, provider headers,
// and verify_api_key's report of the status class only.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "golden/golden.h"
#include "http/testing/fake_http_client.h"
#include "llm/catalog_client.h"
#include "llm/credentials.h"
#include "llm_test_support.h"
#include "pyport/errors.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;

namespace {

constexpr std::string_view kSecret = "sk-test-SECRET-0123456789";

RetryWait no_wait() {
    return [](std::chrono::milliseconds, const CancelToken&) {
    };
}

http::Error connect_failure() {
    return {http::ErrorKind::Connect, "[WinError 10061] No connection could be made because the "
                                      "target machine actively refused it"};
}

// What a vector's key mapping holds, saved in a fresh in-memory store. An empty value is
// "nothing saved", as resolve_credential's falsy answer.
void save_keys(TestKeys& keys, const Json& mapping) {
    for (const auto& [name, value] : mapping.items()) {
        if (!value.get<std::string>().empty()) {
            keys.backend->set(keys.store.service(), name, value.get<std::string>());
        }
    }
}

} // namespace

TEST_CASE("credential status copy points at settings", "[test_credentials]") {
    // from tests/test_credentials.py::test_credential_status_copy_points_at_settings
    TestKeys keys;
    const auto profile = *builtin_profile("openrouter");
    const auto missing = credential_status(profile, keys.sources);
    CHECK_FALSE(missing.available);
    CHECK(missing.text == "Requires OpenRouter API key — add it in Settings → App → API keys");
    keys.store.set("OPENROUTER_API_KEY", kSecret);
    CHECK(credential_status(profile, keys.sources) == KeyStatus{true, "OpenRouter API key found"});
}

TEST_CASE("TestVerifyApiKey success closes client", "[test_credentials]") {
    // from tests/test_credentials.py::TestVerifyApiKey::test_success_closes_client
    // The C++ client is the caller's and stays open; the result and the single request remain.
    http::FakeHttpClient client;
    client.push(json_reply(200, Json{{"object", "list"}, {"data", Json::array()}}));
    const auto check =
        verify_api_key(client, *builtin_profile("openai"), kSecret, {}, nullptr, no_wait());
    CHECK(check.ok);
    CHECK(check.detail == "api.openai.com accepted the key.");
    REQUIRE(client.requests().size() == 1);
    CHECK(client.requests()[0].url == "https://api.openai.com/v1/models");
}

TEST_CASE("TestVerifyApiKey openrouter probes auth endpoint not public catalog",
          "[test_credentials]") {
    // from
    // tests/test_credentials.py::TestVerifyApiKey::test_openrouter_probes_auth_endpoint_not_public_catalog
    http::FakeHttpClient client;
    client.push(json_reply(200, Json{{"data", {{"label", "sk-or-..."}}}}));
    const auto check =
        verify_api_key(client, *builtin_profile("openrouter"), kSecret, {}, nullptr, no_wait());
    CHECK(check.ok);
    CHECK(check.detail == "openrouter.ai accepted the key.");
    const auto requests = client.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].method == "GET");
    CHECK(requests[0].url == "https://openrouter.ai/api/v1/auth/key");
    CHECK(http::find_header(requests[0].headers, "Authorization") ==
          "Bearer " + std::string(kSecret));
    CHECK(http::find_header(requests[0].headers, "X-Title") == std::string(identity().product));
}

TEST_CASE("TestVerifyApiKey failures report status class only", "[test_credentials]") {
    // from tests/test_credentials.py::TestVerifyApiKey::test_failures_report_status_class_only
    const Json echo{
        {"error", {{"message", "Incorrect API key provided: " + std::string(kSecret)}}}};
    struct Failure {
        std::function<void(http::FakeHttpClient&)> script;
        std::string expected;
    };
    const std::vector<Failure> failures{
        {[&](auto& c) { c.push(json_reply(401, echo)); }, "HTTP 401"},
        {[&](auto& c) { c.push(json_reply(403, echo)); }, "HTTP 403"},
        {[&](auto& c) {
             c.set_handler([&](const http::Request&) { return json_reply(502, echo); });
         },
         "HTTP 502"},
        {[](auto& c) {
             c.set_handler([](const http::Request&) {
                 http::ScriptedResponse reply;
                 reply.error = connect_failure();
                 return reply;
             });
         },
         "Couldn't reach"},
        {[](auto& c) {
             c.set_handler([](const http::Request&) -> http::ScriptedResponse {
                 throw py::ValueError(std::string(kSecret));
             });
         },
         "ValueError"},
    };
    for (const auto& failure : failures) {
        LogScope log;
        http::FakeHttpClient client;
        failure.script(client);
        const auto check =
            verify_api_key(client, *builtin_profile("openai"), kSecret, {}, nullptr, no_wait());
        CHECK_FALSE(check.ok);
        CHECK(check.detail.find(failure.expected) != std::string::npos);
        CHECK(check.detail.find(kSecret) == std::string::npos);
        CHECK(log.sink->all().find(kSecret) == std::string::npos);
    }
}

TEST_CASE("an empty key is resolved as the client resolves it", "[credentials]") {
    TestKeys keys;
    http::FakeHttpClient client;
    client.set_handler(
        [](const http::Request&) { return json_reply(200, Json{{"data", Json::array()}}); });
    // A required key that is nowhere is reported by class only.
    CHECK(
        verify_api_key(client, *builtin_profile("groq"), "", {}, &keys.sources, no_wait()).detail ==
        "Verification failed (RuntimeError).");
    CHECK(verify_api_key(client, *builtin_profile("groq"), "", {}, nullptr, no_wait()).detail ==
          "Verification failed (RuntimeError).");
    CHECK(client.requests().empty());
    keys.env.set("GROQ_API_KEY", "gsk-from-the-environment");
    CHECK(verify_api_key(client, *builtin_profile("groq"), "", {}, &keys.sources, no_wait()).ok);
    CHECK(http::find_header(client.requests().back().headers, "Authorization") ==
          "Bearer gsk-from-the-environment");
    // An endpoint that needs no key gets the placeholder.
    CHECK(verify_api_key(client, *get_profile("ollama", Json::object()), "", {}, nullptr, no_wait())
              .ok);
    CHECK(http::find_header(client.requests().back().headers, "Authorization") == "Bearer dummy");
}

TEST_CASE("the connection fingerprint changes with the key", "[credentials]") {
    TestKeys keys;
    const auto profile = *builtin_profile("openrouter");
    const auto before = connection_fingerprint(profile, keys.sources);
    CHECK_FALSE(before.key);
    keys.env.set("OPENROUTER_API_KEY", "sk-or-environment-key");
    const auto after = connection_fingerprint(profile, keys.sources);
    CHECK(after.key == "sk-or-environment-key");
    CHECK_FALSE(before == after);
    CHECK(connection_fingerprint(*get_profile("vllm", Json::object()), keys.sources).key ==
          "dummy");
}

TEST_CASE("credentials replay services.text_llm", "[golden]") {
    const std::string module = "services.text_llm";
    golden::for_each(module, "credential_label", [](const golden::Case& c) {
        CHECK(credential_label(profile_from(c.arg<Json>(0))) == c.result<std::string>());
    });
    golden::for_each(module, "credential_status", [](const golden::Case& c) {
        TestKeys keys;
        save_keys(keys, c.arg<Json>(1));
        const auto status = credential_status(profile_from(c.arg<Json>(0)), keys.sources);
        CHECK(golden::equal(c.result<Json>(), Json::array({status.available, status.text})));
    });
    golden::for_each(module, "resolve_api_key", [](const golden::Case& c) {
        TestKeys keys;
        save_keys(keys, c.arg<Json>(1));
        const auto key = resolve_api_key(profile_from(c.arg<Json>(0)), keys.sources);
        // Python's lookup answers "" for a key saved empty; nothing is saved for it here.
        const auto expected = c.result<std::optional<std::string>>();
        CHECK(key.value_or("") == expected.value_or(""));
    });
    golden::for_each(module, "provider_headers", [](const golden::Case& c) {
        const auto headers = provider_headers(profile_from(c.arg<Json>(0)), c.arg<std::string>(1));
        Json actual = nullptr;
        if (headers) {
            actual = Json::object();
            for (const auto& [name, value] : *headers) {
                actual[name] = value;
            }
        }
        Json expected = c.result<Json>();
        if (expected.is_object()) {
            for (auto& [name, value] : expected.items()) {
                value = golden::rebrand(value.get<std::string>());
            }
        }
        CHECK(golden::equal(expected, actual));
    });
}
