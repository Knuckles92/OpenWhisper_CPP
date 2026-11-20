// services/transcript_cleanup.py: the cleanup client, rule polishing and the cleanup model list,
// against in-process fake providers.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "generation_test_support.h"
#include "golden/golden.h"
#include "http/testing/fake_http_client.h"
#include "llm/catalog_client.h"
#include "llm/cleanup_settings.h"
#include "llm/model_catalog.h"
#include "llm/openai_client.h"
#include "llm/profiles.h"
#include "llm/transcript_cleanup.h"
#include "llm_test_support.h"
#include "pyport/json.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;
using namespace std::chrono_literals;

namespace {

Json chat_answer(const std::string& content) {
    return Json{
        {"choices", Json::array({Json{{"message", {{"role", "assistant"}, {"content", content}}},
                                      {"finish_reason", "stop"}}})}};
}

CleanupOptions quick(std::optional<std::string> key = std::nullopt) {
    CleanupOptions options;
    options.api_key = std::move(key);
    options.retry_hooks.sleep = [](std::chrono::milliseconds, const CancelToken&) {
    };
    return options;
}

// A cleaner over a fake provider, with the Python tests' TranscriptCleanup(...) arguments.
struct Rig {
    std::unique_ptr<TranscriptCleanup> make(std::string_view provider = {},
                                            std::string_view model = {},
                                            std::optional<std::string> key = "test-key",
                                            std::string_view reasoning = {}, bool defer = false) {
        return std::make_unique<TranscriptCleanup>(
            fake, keys.sources, [this] { return settings; }, provider, model, reasoning, defer,
            quick(std::move(key)));
    }
    std::vector<Json> bodies() const {
        std::vector<Json> out;
        for (const auto& request : fake->requests()) {
            out.push_back(request_body(request));
        }
        return out;
    }

    std::shared_ptr<http::FakeHttpClient> fake = std::make_shared<http::FakeHttpClient>();
    TestKeys keys;
    Json settings = Json::object();
};

http::ScriptedResponse failing_with_timeout() {
    http::ScriptedResponse reply;
    reply.error = http::Error{http::ErrorKind::Timeout, "timed out"};
    return reply;
}

} // namespace

TEST_CASE("empty text returns unchanged", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanup::test_empty_text_returns_unchanged
    Rig rig;
    auto cleaner = rig.make();
    CHECK(cleaner->cleanup("") == "");
    CHECK(cleaner->cleanup("   ") == "   ");
    CHECK(rig.fake->requests().empty());
}

TEST_CASE("unavailable returns raw", "[test_transcript_cleanup]") {
    // from tests/test_transcript_cleanup.py::TestTranscriptCleanup::test_unavailable_returns_raw
    Rig rig;
    auto cleaner = rig.make({}, {}, std::nullopt);
    CHECK_FALSE(cleaner->is_available());
    CHECK(cleaner->cleanup("hello um world") == "hello um world");
    CHECK(rig.fake->requests().empty());
}

TEST_CASE("success returns cleaned text", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanup::test_success_returns_cleaned_text
    Rig rig;
    rig.fake->push(json_reply(200, chat_answer("Hello world.")));
    auto cleaner = rig.make();
    CHECK(cleaner->cleanup("hello um world", "Tidy.") == "Hello world.");
    CHECK(rig.fake->requests().size() == 1);
}

TEST_CASE("custom system prompt is sent", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanup::test_custom_system_prompt_is_sent
    Rig rig;
    rig.fake->push(json_reply(200, chat_answer("Done.")));
    auto cleaner = rig.make();
    const std::string custom = "Rewrite as bullet points only.";
    (void)cleaner->cleanup("raw text", custom);
    CHECK(rig.bodies()[0]["messages"][0]["content"] == custom);
}

TEST_CASE("an empty system prompt sends the default prompt", "[transcript_cleanup]") {
    Rig rig;
    rig.fake->push(json_reply(200, chat_answer("Done.")));
    CleanupOptions options = quick("test-key");
    options.default_prompt = "The legacy default.";
    TranscriptCleanup cleaner(
        rig.fake, rig.keys.sources, [] { return Json::object(); }, {}, {}, {}, false, options);
    (void)cleaner.cleanup("raw text", "   ");
    CHECK(rig.bodies()[0]["messages"][0]["content"] == "The legacy default.");
}

TEST_CASE("api error falls back to raw", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanup::test_api_error_falls_back_to_raw
    Rig rig;
    rig.fake->set_handler([](const http::Request&) { return failing_with_timeout(); });
    auto cleaner = rig.make();
    CHECK(cleaner->cleanup("raw transcript", "Tidy.") == "raw transcript");
    CHECK(cleaner->last_error() == "Request timed out.");
}

TEST_CASE("timeout override is sent per request", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanup::test_timeout_override_is_sent_per_request
    // A long job gets its own timeout and keeps the batch retry budget: two failures are
    // retried before the answer.
    Rig rig;
    rig.fake->push(json_reply(500, Json{{"error", "x"}}));
    rig.fake->push(json_reply(500, Json{{"error", "x"}}));
    rig.fake->push(json_reply(200, chat_answer("Done.")));
    auto cleaner = rig.make();
    CHECK(cleaner->cleanup("raw text", "Tidy.", 120.0) == "Done.");
    const auto requests = rig.fake->requests();
    REQUIRE(requests.size() == 3);
    for (const auto& request : requests) {
        CHECK(request.idle_timeout == 120000ms);
    }
}

TEST_CASE("no timeout kwarg when not overridden", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanup::test_no_timeout_kwarg_when_not_overridden
    // The dictation path keeps the client-level timeout and retry cap.
    Rig rig;
    rig.fake->set_handler(
        [](const http::Request&) { return json_reply(500, Json{{"error", "x"}}); });
    auto cleaner = rig.make();
    (void)cleaner->cleanup("raw text", "Tidy.");
    const auto requests = rig.fake->requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].idle_timeout == 8000ms);
}

TEST_CASE("empty model response falls back to raw", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanup::test_empty_model_response_falls_back_to_raw
    Rig rig;
    rig.fake->push(json_reply(200, chat_answer("  ")));
    auto cleaner = rig.make();
    CHECK(cleaner->cleanup("keep me", "Tidy.") == "keep me");
    CHECK(cleaner->last_error() == "empty response");
}

TEST_CASE("last error tracks run outcome", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanup::test_last_error_tracks_run_outcome
    Rig rig;
    auto cleaner = rig.make();
    CHECK(cleaner->last_error() == "not run");
    rig.fake->push(json_reply(200, chat_answer("Cleaned.")));
    (void)cleaner->cleanup("raw text", "Tidy.");
    CHECK_FALSE(cleaner->last_error());
    rig.fake->set_handler([](const http::Request&) { return failing_with_timeout(); });
    (void)cleaner->cleanup("raw text", "Tidy.");
    CHECK(cleaner->last_error());
}

TEST_CASE("last error set when unavailable or empty", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanup::test_last_error_set_when_unavailable_or_empty
    Rig rig;
    auto cleaner = rig.make({}, {}, std::nullopt);
    (void)cleaner->cleanup("hello");
    CHECK(cleaner->last_error() == "cleanup unavailable");
    (void)cleaner->cleanup("");
    CHECK(cleaner->last_error() == "empty input");
}

TEST_CASE("configured model is sent", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanupProviders::test_configured_model_is_sent
    Rig rig;
    rig.fake->push(json_reply(200, chat_answer("Cleaned.")));
    auto cleaner = rig.make({}, "my-model");
    (void)cleaner->cleanup("raw text", "Tidy.");
    CHECK(rig.bodies()[0]["model"] == "my-model");
}

TEST_CASE("reasoning off sends temperature zero", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanupProviders::test_reasoning_off_sends_temperature_zero
    Rig rig;
    rig.fake->push(json_reply(200, chat_answer("Cleaned.")));
    auto cleaner = rig.make({}, {}, "k");
    (void)cleaner->cleanup("raw text", "Tidy.");
    const Json body = rig.bodies()[0];
    CHECK(body["temperature"] == 0);
    CHECK_FALSE(body.contains("reasoning_effort"));
}

TEST_CASE("reasoning openai sends responses reasoning effort", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanupProviders::test_reasoning_openai_sends_responses_reasoning_effort
    Rig rig;
    rig.fake->push(json_reply(
        200, Json{{"status", "completed"},
                  {"output",
                   Json::array({Json{{"type", "message"},
                                     {"content", Json::array({Json{{"type", "output_text"},
                                                                   {"text", "Cleaned."}}})}}})}}));
    auto cleaner = rig.make("openai", "o4-mini", "k", "high");
    CHECK(cleaner->cleanup("raw text", "Tidy.") == "Cleaned.");
    const auto requests = rig.fake->requests();
    REQUIRE(requests.size() == 1);
    CHECK(url_path(requests[0].url) == "/v1/responses");
    const Json body = request_body(requests[0]);
    CHECK(body["reasoning"] == Json{{"effort", "high"}});
    CHECK(body["store"] == false);
    CHECK_FALSE(body.contains("temperature"));
}

TEST_CASE("reasoning openrouter uses extra body", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanupProviders::test_reasoning_openrouter_uses_extra_body
    // extra_body's fields reach the request body itself.
    Rig rig;
    rig.fake->push(json_reply(200, chat_answer("Cleaned.")));
    auto cleaner = rig.make("openrouter", {}, "k", "low");
    (void)cleaner->cleanup("raw text", "Tidy.");
    const Json body = rig.bodies()[0];
    CHECK(body["reasoning"] == Json{{"effort", "low"}});
    CHECK_FALSE(body.contains("temperature"));
}

TEST_CASE("configure switches provider and model", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanupProviders::test_configure_switches_provider_and_model
    Rig rig;
    auto cleaner = rig.make({}, {}, "openai-key");
    rig.keys.env.set("OPENROUTER_API_KEY", "router-key");
    cleaner->configure("openrouter", "meta-llama/llama-3-8b", "medium");
    CHECK(cleaner->provider() == "openrouter");
    CHECK(cleaner->model() == "meta-llama/llama-3-8b");
    CHECK(cleaner->reasoning() == "medium");
    CHECK(cleaner->api_key() == "router-key");
    CHECK(cleaner->is_available());
}

TEST_CASE("configure same provider keeps client", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanupProviders::test_configure_same_provider_keeps_client
    Rig rig;
    auto cleaner = rig.make("openai", {}, "openai-key");
    const auto original = cleaner->client_generation();
    REQUIRE(original != 0);
    cleaner->configure("openai", "gpt-4.1-mini");
    CHECK(cleaner->client_generation() == original);
    CHECK(cleaner->model() == "gpt-4.1-mini");
}

TEST_CASE("empty instruction returns error", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestPolishCleanupRule::test_empty_instruction_returns_error
    Rig rig;
    const auto [rule, error] = polish_cleanup_rule("   ", "Polish.", rig.fake, rig.keys.sources,
                                                   [] { return Json::object(); });
    CHECK(rule.empty());
    CHECK(error == "empty instruction");
    CHECK(rig.fake->requests().empty());
}

TEST_CASE("unavailable falls back to verbatim", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestPolishCleanupRule::test_unavailable_falls_back_to_verbatim
    Rig rig;
    const auto [rule, error] =
        polish_cleanup_rule("  always spell my name Alex  ", "Polish.", rig.fake, rig.keys.sources,
                            [] { return Json::object(); });
    CHECK(rule == "always spell my name Alex");
    CHECK(error);
    CHECK(rig.fake->requests().empty());
}

TEST_CASE("success returns polished rule with polish prompt", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestPolishCleanupRule::test_success_returns_polished_rule_with_polish_prompt
    // The polish prompt is services.cleanup_prompts' text, which the caller passes.
    const std::string polish_prompt = "You convert a user's instruction into one short rule.";
    Rig rig;
    rig.keys.env.set("OPENROUTER_API_KEY", "test-key");
    rig.fake->push(json_reply(200, chat_answer("Always spell the user's name \"Alex Rivera\".")));
    const auto [rule, error] =
        polish_cleanup_rule("um so my name should always be spelled Alex Rivera", polish_prompt,
                            rig.fake, rig.keys.sources, [] { return Json::object(); });
    CHECK(rule == "Always spell the user's name \"Alex Rivera\".");
    CHECK_FALSE(error);
    CHECK(rig.bodies()[0]["messages"][0]["content"] == polish_prompt);
    CHECK(rig.bodies()[0]["messages"][1]["content"] ==
          "um so my name should always be spelled Alex Rivera");
}

TEST_CASE("api error falls back to verbatim", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestPolishCleanupRule::test_api_error_falls_back_to_verbatim
    Rig rig;
    rig.keys.env.set("OPENROUTER_API_KEY", "test-key");
    rig.fake->set_handler([](const http::Request&) { return failing_with_timeout(); });
    CleanupOptions options = quick();
    const auto [rule, error] = polish_cleanup_rule(
        "expand SCWA", "Polish.", rig.fake, rig.keys.sources, [] { return Json::object(); },
        std::nullopt, std::nullopt, std::nullopt, {}, options);
    CHECK(rule == "expand SCWA");
    CHECK(error);
}

namespace {

Json custom_settings(const std::string& url, const std::string& name = "Local") {
    return Json{
        {"text_llm_profiles",
         Json::array({Json{
             {"id", "custom_abcd1234"}, {"name", name}, {"base_url", url}, {"api_key_env", ""}}})}};
}

} // namespace

TEST_CASE("configure rebuilds when endpoint url changes", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestCleanupCustomEndpoints::test_configure_rebuilds_when_endpoint_url_changes
    Rig rig;
    rig.settings = custom_settings("http://127.0.0.1:1234/v1");
    std::atomic<int> built{0};
    CleanupOptions options = quick();
    options.client_factory = [&](const TextLLMProfile& profile, double timeout,
                                 const std::string& key) {
        ++built;
        return OpenAiCompatClient(rig.fake, profile.base_url.value_or(""), key, {}, timeout, 1);
    };
    TranscriptCleanup cleaner(
        rig.fake, rig.keys.sources, [&] { return rig.settings; }, "custom_abcd1234", "local-qwen",
        {}, false, options);
    const auto first_client = cleaner.client_generation();
    const auto first_connection = cleaner.connection();
    CHECK(cleaner.is_available());
    rig.settings["text_llm_profiles"][0]["base_url"] = "http://127.0.0.1:8000/v1";
    cleaner.configure("custom_abcd1234", "local-qwen");
    CHECK(cleaner.client_generation() != first_client);
    CHECK(cleaner.connection() != first_connection);
    CHECK(built >= 2);
}

TEST_CASE("servers the user runs get time to load a model", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestCleanupCustomEndpoints::test_servers_the_user_runs_get_time_to_load_a_model
    const std::vector<std::tuple<std::string, Json, double>> table{
        {"custom_abcd1234", custom_settings("http://localhost:1234/v1"), 120.0},
        {"vllm", Json{{"vllm_base_url", "http://192.168.1.50:8000/v1"}}, 120.0},
        {"custom_abcd1234", custom_settings("https://gateway.test/v1", "Gateway"),
         kTranscriptCleanupTimeoutS},
    };
    for (const auto& [provider, settings, timeout] : table) {
        INFO(provider << " " << py::dumps(settings));
        Rig rig;
        rig.settings = settings;
        auto cleaner = rig.make(provider, "qwen3", std::nullopt);
        CHECK(cleaner->attempt_timeout_s() == timeout);
    }
}

TEST_CASE("list cleanup models uses custom profile", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestCleanupCustomEndpoints::test_list_cleanup_models_uses_custom_profile
    forget_server_specs();
    http::FakeHttpClient server;
    server.set_handler([](const http::Request& request) {
        if (request.method == "GET" && url_path(request.url) == "/v1/models") {
            return json_reply(
                200, Json{{"data", Json::array({Json{{"id", "beta"}}, Json{{"id", "alpha"}}})}});
        }
        return text_reply(404, "Not Found");
    });
    TestKeys keys;
    CHECK(list_cleanup_models(server, "custom_abcd1234", std::nullopt,
                              custom_settings("http://127.0.0.1:1234/v1"),
                              keys.sources) == std::vector<std::string>{"alpha", "beta"});
    for (const auto& request : server.requests()) {
        CHECK(request.url.starts_with("http://127.0.0.1:1234/"));
    }
    try {
        (void)list_cleanup_models(server, "custom_missing", std::nullopt, Json::object(),
                                  keys.sources);
        FAIL("listed an unknown endpoint");
    } catch (const CatalogError& error) {
        CHECK(std::string(error.what()) == "Unknown text model endpoint 'custom_missing'");
    }
    forget_server_specs();
}

TEST_CASE("list cleanup models catalog failure is raised", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestCleanupCustomEndpoints::test_list_cleanup_models_catalog_failure_is_raised
    forget_server_specs();
    http::FakeHttpClient server;
    server.set_handler([](const http::Request&) { return text_reply(500, "catalog down"); });
    TestKeys keys;
    CHECK_THROWS_AS(list_cleanup_models(server, "custom_abcd1234", std::nullopt,
                                        custom_settings("http://127.0.0.1:1234/v1"), keys.sources),
                    CatalogError);
    forget_server_specs();
}

TEST_CASE("deferred cleanup builds its client on first configure", "[test_openai_lazy_import]") {
    // from
    // tests/test_openai_lazy_import.py::test_deferred_cleanup_builds_its_client_on_first_configure
    Rig rig;
    rig.keys.env.set("OPENAI_API_KEY", "sk-test");
    std::atomic<int> created{0};
    CleanupOptions options = quick();
    options.client_factory = [&](const TextLLMProfile& profile, double timeout,
                                 const std::string& key) {
        ++created;
        return OpenAiCompatClient(rig.fake, profile.base_url.value_or(""), key, {}, timeout, 1);
    };
    TranscriptCleanup cleaner(
        rig.fake, rig.keys.sources, [] { return Json::object(); }, "openai", {}, {}, true, options);
    CHECK(cleaner.client_generation() == 0);
    CHECK(created == 0);
    cleaner.configure("openai", "gpt-test");
    CHECK(created == 1);
    CHECK(cleaner.is_available());
    cleaner.configure("openai", "gpt-test");
    CHECK(created == 1);
}

TEST_CASE("an installed agent provider is kept and reads as unavailable", "[transcript_cleanup]") {
    Rig rig;
    auto cleaner = rig.make("codex", "gpt-5", std::nullopt, {}, false);
    cleaner->configure("codex", "gpt-5", "high");
    CHECK(cleaner->provider() == "codex");
    CHECK(cleaner->attempt_timeout_s() == kAgentCleanupTimeoutS);
    CHECK_FALSE(cleaner->is_available());
    CHECK(cleaner->cleanup("raw", "Tidy.") == "raw");
    CHECK(cleaner->last_error() == "cleanup unavailable");
    CHECK(rig.fake->requests().empty());
}

TEST_CASE("the attempt budget matches OpenWhisper's", "[golden]") {
    golden::for_each("services.transcript_cleanup", "_attempt_budget", [](const golden::Case& c) {
        const auto budget = attempt_budget(c.arg<double>(0), c.arg<std::optional<double>>(1),
                                           c.arg<std::optional<double>>(2));
        const Json expected = c.result<Json>();
        CHECK(budget.give_up_s == golden::decode<double>(expected.at("give_up_s")));
        if (expected.at("batch_retries").is_null()) {
            CHECK_FALSE(budget.overridden);
            CHECK(budget.max_retries == kTranscriptCleanupMaxRetries);
        } else {
            CHECK(budget.overridden);
            CHECK(budget.max_retries == expected.at("batch_retries").get<int>());
        }
    });
}

TEST_CASE("provider env keys match OpenWhisper's", "[golden]") {
    golden::for_each("services.transcript_cleanup", "provider_env_key", [](const golden::Case& c) {
        CHECK(provider_env_key(c.arg<std::string>(0), c.arg<Json>(1)) == c.result<std::string>());
    });
}

TEST_CASE("cleanup sends and returns what OpenWhisper's does", "[golden]") {
    golden::for_each("services.transcript_cleanup", "cleanup", [](const golden::Case& c) {
        forget_server_specs();
        const Json input = c.arg<Json>(0);
        const Json expected = c.result<Json>();
        auto fake = std::make_shared<http::FakeHttpClient>();
        const Json reply = input.at("reply");
        fake->set_handler([reply](const http::Request&) {
            return json_reply(reply.at("status").get<int>(), reply.at("json"));
        });
        TestKeys keys;
        const auto configured_keys = input.value("keys", Json::object());
        for (const auto& [name, value] : configured_keys.items()) {
            keys.env.set(name, value.get<std::string>());
        }
        const Json settings = input.value("settings", Json::object());
        const auto text_of = [&](const char* key) {
            const auto found = input.find(key);
            return found == input.end() || found->is_null() ? std::string()
                                                            : found->get<std::string>();
        };
        TranscriptCleanup cleaner(
            fake, keys.sources, [settings] { return settings; }, text_of("provider"),
            text_of("model"), text_of("reasoning"), false, quick(std::string(kSecret)));
        const auto optional_of = [&](const char* key) -> std::optional<double> {
            const auto found = input.find(key);
            if (found == input.end() || found->is_null()) {
                return std::nullopt;
            }
            return found->get<double>();
        };
        const std::string text = cleaner.cleanup(
            input.at("text").get<std::string>(), input.at("system_prompt").get<std::string>(),
            optional_of("timeout_s"), optional_of("deadline_s"));
        CHECK(text == expected.at("text").get<std::string>());
        const auto error = cleaner.last_error();
        CHECK(golden::equal(expected.at("last_error"), error ? Json(*error) : Json()));
        CHECK(cleaner.model() == expected.at("model").get<std::string>());
        CHECK(cleaner.reasoning() == expected.at("reasoning").get<std::string>());
        CHECK(cleaner.attempt_timeout_s() == expected.at("attempt_timeout_s").get<double>());
        const auto requests = fake->requests();
        REQUIRE(requests.size() == expected.at("requests").size());
        for (std::size_t i = 0; i < requests.size(); ++i) {
            const Json actual = recorded_request(requests[i], true, false);
            INFO(py::dumps(actual));
            CHECK(same_request(expected.at("requests")[i], actual));
        }
        forget_server_specs();
    });
}

namespace {

Json agent_cleanup_settings(std::string_view provider) {
    return Json{{"transcript_cleanup_enabled", true},
                {"transcript_cleanup_provider", std::string(provider)},
                {"transcript_cleanup_model", ""},
                {"fv_cleanup_agent_consent", std::string(provider)}};
}

} // namespace

TEST_CASE("authorized transcript cleanup submits the selected agent request",
          "[transcript_cleanup]") {
    Rig rig;
    rig.settings = agent_cleanup_settings("opencode_cli");
    std::optional<CleanupAgentRequest> request;
    CleanupOptions options = quick("ignored-key");
    options.agent_available = [](std::string_view provider) {
        return provider == "opencode_cli";
    };
    options.run_agent = [&](const CleanupAgentRequest& submitted, const CancelToken&) {
        request = submitted;
        return "  Finished.  ";
    };
    TranscriptCleanup cleaner(
        rig.fake, rig.keys.sources, [&] { return rig.settings; }, "opencode_cli", "", "off", true,
        options);
    REQUIRE(cleaner.is_available());
    CHECK_FALSE(cleaner.api_key());
    CHECK(cleaner.attempt_timeout_s() == 60.0);
    CHECK(cleaner.cleanup("raw one", "  Use these rules.  ", 8.0, 4.0) == "Finished.");
    CHECK_FALSE(cleaner.last_error());
    REQUIRE(request);
    CHECK(request->provider == "opencode_cli");
    CHECK(request->model.empty());
    CHECK(request->text == "raw one");
    CHECK(request->system_prompt == "Use these rules.");
    CHECK(request->reasoning == "low");
    CHECK(request->timeout_s == 4.0);

    cleaner.configure("opencode_cli", "chosen-model", "high");
    CHECK(cleaner.cleanup("raw two", "", 7.0) == "Finished.");
    REQUIRE(request);
    CHECK(request->model == "chosen-model");
    CHECK(request->reasoning == "high");
    CHECK(request->timeout_s == 7.0);
    CHECK(rig.fake->requests().empty());
}

TEST_CASE("installed agent cleanup checks current consent before dispatch",
          "[transcript_cleanup]") {
    Rig rig;
    rig.settings = agent_cleanup_settings("codex");
    int discoveries = 0;
    int invocations = 0;
    CleanupOptions options;
    options.agent_available = [&](std::string_view) {
        ++discoveries;
        if (discoveries == 1) {
            rig.settings["fv_cleanup_agent_consent"] = "";
        }
        return true;
    };
    options.run_agent = [&](const CleanupAgentRequest&,
                            const CancelToken&) -> std::optional<std::string> {
        ++invocations;
        return "changed";
    };
    TranscriptCleanup cleaner(
        rig.fake, rig.keys.sources, [&] { return rig.settings; }, "codex", "", "", true, options);
    SECTION("pre-cancelled") {
        CancelSource cancel;
        cancel.cancel();
        CHECK(cleaner.cleanup("raw", {}, {}, {}, cancel.token()) == "raw");
        CHECK(cleaner.last_error() == kCanceledReason);
        CHECK(discoveries == 0);
    }
    SECTION("stale provider consent") {
        rig.settings["transcript_cleanup_provider"] = "claude_code";
        CHECK_FALSE(cleaner.is_available());
        CHECK(cleaner.cleanup("raw") == "raw");
        CHECK(cleaner.last_error() == "cleanup unavailable");
        CHECK(discoveries == 0);
    }
    SECTION("revoked during discovery") {
        CHECK(cleaner.cleanup("raw") == "raw");
        CHECK(cleaner.last_error() == "cleanup unavailable");
        CHECK(discoveries == 1);
    }
    CHECK(invocations == 0);
    CHECK(rig.fake->requests().empty());
}

TEST_CASE("installed agent failure and empty output preserve raw transcript",
          "[transcript_cleanup]") {
    Rig rig;
    rig.settings = agent_cleanup_settings("codex");
    CleanupOptions options;
    options.agent_available = [](std::string_view) {
        return true;
    };
    std::string expected;
    SECTION("unavailable") {
        options.agent_available = [](std::string_view) {
            return false;
        };
        options.run_agent = [](const CleanupAgentRequest&, const CancelToken&) {
            return std::optional<std::string>("unexpected result");
        };
        expected = "cleanup unavailable";
    }
    SECTION("failed") {
        options.run_agent = [](const CleanupAgentRequest&, const CancelToken&) {
            return std::optional<std::string>();
        };
        expected = "Agent cleanup failed";
    }
    SECTION("empty") {
        options.run_agent = [](const CleanupAgentRequest&, const CancelToken&) {
            return std::optional<std::string>("  ");
        };
        expected = "empty response";
    }
    SECTION("throwing runner") {
        options.run_agent = [](const CleanupAgentRequest&,
                               const CancelToken&) -> std::optional<std::string> {
            throw std::runtime_error("provider error with transcript content");
        };
        expected = "Agent cleanup failed";
    }
    TranscriptCleanup cleaner(
        rig.fake, rig.keys.sources, [&] { return rig.settings; }, "codex", "", "", true, options);
    CHECK(cleaner.cleanup("  raw transcript  ") == "  raw transcript  ");
    CHECK(cleaner.last_error() == expected);
    CHECK(rig.fake->requests().empty());
}

TEST_CASE("installed agent worker is stopped on cancellation or cleanup deadline",
          "[transcript_cleanup]") {
    Rig rig;
    rig.settings = agent_cleanup_settings("codex");
    FakeClock clock;
    CancelSource caller;
    std::atomic<bool> saw_cancel = false;
    bool cancel_on_start = false;
    SECTION("caller cancellation") {
        cancel_on_start = true;
    }
    SECTION("cleanup deadline") {}
    CleanupOptions options;
    options.clock = &clock;
    options.agent_available = [](std::string_view) {
        return true;
    };
    options.run_agent = [&](const CleanupAgentRequest&, const CancelToken& cancel) {
        if (cancel_on_start) {
            caller.cancel();
        } else {
            clock.advance(200ms);
        }
        saw_cancel = cancel.wait_for(2s);
        return std::optional<std::string>("late reply");
    };
    {
        TranscriptCleanup cleaner(
            rig.fake, rig.keys.sources, [&] { return rig.settings; }, "codex", "", "", true,
            options);
        CHECK(cleaner.cleanup("raw", {}, {}, cancel_on_start ? 2.0 : 0.2, caller.token()) == "raw");
        REQUIRE(cleaner.last_error());
        if (cancel_on_start) {
            CHECK(cleaner.last_error() == kCanceledReason);
        } else {
            CHECK(cleaner.last_error()->starts_with("timed out"));
        }
    }
    CHECK(saw_cancel.load());
    CHECK(rig.fake->requests().empty());
}
