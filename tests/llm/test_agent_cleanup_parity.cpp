#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>
#include <optional>
#include <string>

#include "http/testing/fake_http_client.h"
#include "llm/cleanup_settings.h"
#include "llm/transcript_cleanup.h"
#include "llm_test_support.h"

TEST_CASE("every installed cleanup provider retains its chosen model without HTTP",
          "[transcript_cleanup]") {
    const auto provider =
        GENERATE(std::string("claude_code"), std::string("codex"), std::string("opencode_cli"));
    const auto model = GENERATE(std::string(), std::string("chosen-model"));
    fv::Json settings{{"transcript_cleanup_enabled", true},
                      {"transcript_cleanup_provider", provider},
                      {"transcript_cleanup_model", model},
                      {"fv_cleanup_agent_consent", provider}};
    CHECK(fv::llm::resolve_transcript_cleanup_provider(settings) == provider);
    CHECK(fv::llm::resolve_transcript_cleanup_model(settings) == model);
    fv::llm::test::TestKeys keys;
    auto http = std::make_shared<fv::http::FakeHttpClient>();
    std::optional<fv::llm::CleanupAgentRequest> received;
    fv::llm::CleanupOptions options;
    options.agent_available = [&](std::string_view name) {
        return name == provider;
    };
    options.run_agent = [&](const fv::llm::CleanupAgentRequest& request,
                            const fv::CancelToken&) -> std::optional<std::string> {
        received = request;
        return "Hello world.";
    };
    fv::llm::TranscriptCleanup cleaner(
        http, keys.sources, [&settings] { return settings; }, provider, model, "off", true,
        std::move(options));
    REQUIRE(cleaner.is_available());
    CHECK_FALSE(cleaner.api_key());
    CHECK(cleaner.cleanup("hello um world", "Use the profile's rules.") == "Hello world.");
    CHECK_FALSE(cleaner.last_error());
    REQUIRE(received);
    CHECK(received->provider == provider);
    CHECK(received->model == model);
    CHECK(received->text == "hello um world");
    CHECK(received->system_prompt == "Use the profile's rules.");
    CHECK(received->reasoning == "low");
    CHECK(http->requests().empty());
}

TEST_CASE("switching cleanup from an API client to an approved agent drops the API client",
          "[transcript_cleanup]") {
    fv::llm::test::TestKeys keys;
    auto http = std::make_shared<fv::http::FakeHttpClient>();
    fv::Json settings{{"transcript_cleanup_enabled", true},
                      {"transcript_cleanup_provider", "openai"},
                      {"transcript_cleanup_model", "gpt-test"}};
    std::optional<fv::llm::CleanupAgentRequest> received;
    fv::llm::CleanupOptions options;
    options.api_key = "fake-key";
    options.agent_available = [](std::string_view provider) {
        return provider == "opencode_cli";
    };
    options.run_agent = [&](const fv::llm::CleanupAgentRequest& request,
                            const fv::CancelToken&) -> std::optional<std::string> {
        received = request;
        return "Cleaned.";
    };
    fv::llm::TranscriptCleanup cleaner(
        http, keys.sources, [&settings] { return settings; }, "openai", "gpt-test", "off", false,
        std::move(options));
    REQUIRE(cleaner.client_generation() != 0);
    settings["transcript_cleanup_provider"] = "opencode_cli";
    settings["transcript_cleanup_model"] = "";
    settings["fv_cleanup_agent_consent"] = "opencode_cli";
    cleaner.configure("opencode_cli", "", "high");
    CHECK(cleaner.client_generation() == 0);
    CHECK_FALSE(cleaner.api_key());
    CHECK(cleaner.cleanup("raw") == "Cleaned.");
    REQUIRE(received);
    CHECK(received->provider == "opencode_cli");
    CHECK(received->reasoning == "high");
    CHECK(http->requests().empty());
}
