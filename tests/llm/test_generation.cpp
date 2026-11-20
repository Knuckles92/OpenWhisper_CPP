// services/text_generation.py: the four protocol adapters against in-memory providers, and the
// local-server request shapes of tests/test_local_text_servers.py. Nothing leaves the process.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "base/identity.h"
#include "generation_test_support.h"
#include "golden/golden.h"
#include "http/testing/fake_http_client.h"
#include "llm/catalog_client.h"
#include "llm/generation.h"
#include "llm/local_servers.h"
#include "llm/model_catalog.h"
#include "llm/openai_client.h"
#include "llm/profiles.h"
#include "llm/transcript_cleanup.h"
#include "llm_test_support.h"
#include "pyport/json.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;

namespace {

struct Case {
    std::string provider;
    std::string model;
    std::string protocol;
    std::string path;
};

// tests/test_text_generation.py CASES.
const std::vector<Case>& cases() {
    static const std::vector<Case> table{
        {"openai", "gpt-6-luna", "responses", "/v1/responses"},
        {"openai", "o4-mini", "responses", "/v1/responses"},
        {"openai", "gpt-5.6-luna", "responses", "/v1/responses"},
        {"openai", "gpt-4o-mini", "responses", "/v1/responses"},
        {"openai", "gpt-4.1-mini", "responses", "/v1/responses"},
        {"openai", "gpt-3.5-turbo", "chat", "/v1/chat/completions"},
        {"ollama", "llama3.2", "chat", "/v1/chat/completions"},
        {"vllm", "qwen3-8b", "chat", "/v1/chat/completions"},
        {"groq", "llama-3.3-70b-versatile", "chat", "/openai/v1/chat/completions"},
        {"opencode_go", "glm-5.2", "chat", "/zen/go/v1/chat/completions"},
        {"opencode_go", "gpt-5.6-luna", "responses", "/zen/go/v1/responses"},
        {"opencode_go", "minimax-m3", "anthropic", "/zen/go/v1/messages"},
        {"opencode_zen", "glm-5.2", "chat", "/zen/v1/chat/completions"},
        {"opencode_zen", "gpt-5.6-luna", "responses", "/zen/v1/responses"},
        {"opencode_zen", "claude-sonnet-4-6", "anthropic", "/zen/v1/messages"},
        {"opencode_zen", "gemini-3.1-pro", "google",
         "/zen/v1/models/gemini-3.1-pro:generateContent"},
    };
    return table;
}

Json messages_of(std::initializer_list<std::pair<const char*, const char*>> items) {
    Json out = Json::array();
    for (const auto& [role, content] : items) {
        out.push_back(Json{{"role", role}, {"content", content}});
    }
    return out;
}

// tests/test_text_generation.py client_for: the SDK client with no retries and no default
// headers, over a fake provider answering `replies` in order.
struct Provider {
    Provider(const std::string& id, std::vector<Json> replies, std::string key = "test-secret")
        : profile(*get_profile(id, Json::object())) {
        for (const auto& reply : replies) {
            fake->push(json_reply(200, reply));
        }
        client.emplace(fake, profile.base_url.value_or(""), key, http::Headers{}, 600.0, 0);
    }
    Provider(TextLLMProfile given, std::vector<Json> replies, std::string key = "dummy")
        : profile(std::move(given)) {
        for (const auto& reply : replies) {
            fake->push(json_reply(200, reply));
        }
        client.emplace(fake, profile.base_url.value_or(""), key, http::Headers{}, 600.0, 0);
    }

    std::vector<Json> bodies() const {
        std::vector<Json> out;
        for (const auto& request : fake->requests()) {
            out.push_back(request_body(request));
        }
        return out;
    }

    TextLLMProfile profile;
    std::shared_ptr<http::FakeHttpClient> fake = std::make_shared<http::FakeHttpClient>();
    std::optional<OpenAiCompatClient> client;
};

Json reply_for(const std::string& protocol, bool tool = false) {
    return reply_body(tool ? protocol + "_tool" : protocol);
}

// An http::Client that sets a cancel source while the provider answers, then delivers the
// answer: the response arrives and only the check after it can refuse it.
class CancelingClient final : public http::Client {
public:
    CancelingClient(CancelSource source, Json reply)
        : source_(std::move(source)), reply_(std::move(reply)) {}
    std::expected<http::Response, http::Error> send(const http::Request&, const CancelToken&,
                                                    http::BodySink*) override {
        source_.cancel();
        http::Response response;
        response.status = 200;
        response.headers = {{"Content-Type", "application/json"}};
        response.body = py::dumps(reply_);
        return response;
    }

private:
    CancelSource source_;
    Json reply_;
};

// Runs a scenario of scenarios.json as the vector's WRAPPER does.
Json run_scenario(const Json& scenario) {
    const Json& document = generation_fixture("scenarios");
    const TextLLMProfile profile = scenario.at("profile").is_string()
                                       ? *get_profile(scenario.at("profile").get<std::string>(),
                                                      scenario.value("settings", Json::object()))
                                       : profile_from(scenario.at("profile"));
    forget_server_specs();
    if (scenario.contains("server_specs")) {
        std::map<std::string, TextModelSpec> specs;
        for (const auto& [model, fields] : scenario.at("server_specs").items()) {
            specs.emplace(model, spec_from_fields(fields));
        }
        remember_server_specs(profile.base_url.value_or(""), std::move(specs));
    }
    CancelSource cancel;
    const bool cancel_on_reply = scenario.value("cancel_on_reply", false);
    auto fake = std::make_shared<http::FakeHttpClient>();
    auto replies = std::make_shared<std::vector<Json>>();
    for (const auto& entry : scenario.at("replies")) {
        replies->push_back(entry);
    }
    auto next = std::make_shared<std::size_t>(0);
    fake->set_handler([=](const http::Request&) mutable {
        const Json entry = replies->at((*next)++);
        if (cancel_on_reply) {
            cancel.cancel();
        }
        return scripted_reply(entry);
    });
    auto client = quick_client(fake, profile, scenario.value("max_retries", 0));

    std::optional<Json> tools;
    if (scenario.contains("tools")) {
        const Json& given = scenario.at("tools");
        if (given.is_boolean() && given.get<bool>()) {
            tools = Json::array({document.at("tool")});
        } else if (given.is_array() && !given.empty()) {
            tools = given;
        }
    }
    Json messages = scenario.value("messages", document.at("default_messages"));
    Json turns = Json::array();
    const int count = scenario.contains("tool_result") ? 2 : 1;
    for (int turn = 0; turn < count; ++turn) {
        GenerateRequest request;
        request.model = scenario.at("model").get<std::string>();
        request.messages = messages;
        request.tools = tools;
        request.cancel = cancel.token();
        request.session_id = scenario.value("session_id", std::string());
        request.reasoning_level = scenario.value("reasoning_level", std::string());
        request.options = scenario.value("options", Json::object());
        try {
            const TextResult result = generate(client, profile, std::move(request));
            turns.push_back(result_json(result));
            if (turn == 0 && scenario.contains("tool_result")) {
                messages.push_back(result.assistant_message);
                messages.push_back(Json{{"role", "tool"},
                                        {"tool_call_id", result.tool_calls.at(0).id},
                                        {"content", scenario.at("tool_result")}});
            }
        } catch (...) {
            turns.push_back(failure_json(std::current_exception()));
            break;
        }
    }
    Json requests = Json::array();
    for (const auto& request : fake->requests()) {
        requests.push_back(recorded_request(request));
    }
    forget_server_specs();
    return Json{{"turns", std::move(turns)}, {"requests", std::move(requests)}};
}

} // namespace

TEST_CASE("generate sends and reads what OpenWhisper's SDK client does", "[golden]") {
    golden::for_each("services.text_generation", "generate", [](const golden::Case& c) {
        const auto name = c.arg<std::string>(0);
        INFO(name);
        const Json& scenario = generation_fixture("scenarios").at("scenarios").at(name);
        const Json expected = c.result<Json>();
        const Json actual = run_scenario(scenario);
        CHECK(golden::equal(expected.at("turns"), actual.at("turns")));
        REQUIRE(expected.at("requests").size() == actual.at("requests").size());
        for (std::size_t i = 0; i < actual.at("requests").size(); ++i) {
            INFO("request " << i << ": " << py::dumps(actual.at("requests")[i]));
            CHECK(same_request(expected.at("requests")[i], actual.at("requests")[i]));
        }
    });
}

TEST_CASE("wire protocol and signed tool roundtrip", "[test_text_generation]") {
    // from tests/test_text_generation.py::test_wire_protocol_and_signed_tool_roundtrip
    for (const auto& [provider, model, protocol, path] : cases()) {
        INFO(provider << " " << model);
        Provider server(provider, {reply_for(protocol, true), reply_for(protocol)});
        Json messages = messages_of({{"system", "Use evidence."}, {"user", "Budget?"}});
        GenerateRequest request{model, messages, Json::array({tool_schema()}), {}, "meeting-1"};
        const auto first = generate(*server.client, server.profile, request);
        REQUIRE(first.tool_calls.at(0).id == "call_1");
        messages.push_back(first.assistant_message);
        messages.push_back(
            Json{{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", "{\"hits\":[]}"}});
        request.messages = messages;
        const auto final_turn = generate(*server.client, server.profile, request);
        CHECK(final_turn.text == "Clean text.");
        const auto requests = server.fake->requests();
        REQUIRE(requests.size() == 2);
        for (const auto& sent : requests) {
            CHECK(url_path(sent.url) == path);
            CHECK(http::find_header(sent.headers, "authorization") == "Bearer test-secret");
            if (provider == "opencode_go") {
                CHECK(http::find_header(sent.headers, "x-opencode-session") == "meeting-1");
                CHECK(http::find_header(sent.headers, "user-agent") ==
                      std::string(identity().product));
            } else {
                CHECK_FALSE(http::find_header(sent.headers, "x-opencode-session"));
            }
        }
        const Json body = server.bodies()[1];
        const std::string encoded = py::dumps(body);
        CHECK(encoded.find("call_1") != std::string::npos);
        CHECK(encoded.find("hits") != std::string::npos);
        CHECK(encoded.find("_google") == std::string::npos);
        CHECK(encoded.find("_anthropic") == std::string::npos);
        CHECK(encoded.find("_responses") == std::string::npos);
        if (protocol == "chat") {
            CHECK(body["messages"][2]["reasoning_content"] == "private reasoning");
        } else if (protocol == "responses") {
            CHECK(body["input"][2]["encrypted_content"] == "signed-reasoning");
            CHECK(body["input"].back()["type"] == "function_call_output");
            CHECK(body["tools"][0]["strict"] == false);
        } else if (protocol == "anthropic") {
            CHECK(body["messages"][1]["content"][0]["signature"] == "signed-reasoning");
            CHECK(body["messages"].back()["content"][0]["tool_use_id"] == "call_1");
        } else {
            CHECK(body["contents"][1]["parts"][1]["thoughtSignature"] == "tool-signature");
            CHECK(body["contents"].back()["parts"][0]["functionResponse"]["name"] ==
                  "search_context_files");
        }
    }
}

namespace {

// A TranscriptCleanup whose requests go to `provider` (as the Python tests patch
// create_openai_client) and whose server discovery finds nothing.
struct CleanupRig {
    explicit CleanupRig(std::shared_ptr<http::FakeHttpClient> provider_fake,
                        std::string base_url = {}, int max_retries = 0)
        : provider(std::move(provider_fake)) {
        discovery->set_handler([](const http::Request&) { return text_reply(404, "Not Found"); });
        options.client_factory = [this, base_url, max_retries](const TextLLMProfile& profile,
                                                               double timeout, const std::string&) {
            OpenAiCompatClient client(provider,
                                      base_url.empty() ? profile.base_url.value_or("") : base_url,
                                      "test-secret", {}, timeout, max_retries);
            client.set_hooks(
                {[](std::chrono::milliseconds, const CancelToken&) {}, nullptr, nullptr});
            return client;
        };
    }
    std::unique_ptr<TranscriptCleanup> make(std::string_view id, std::string_view model,
                                            std::string_view reasoning = {},
                                            std::optional<std::string> key = "test-secret") {
        CleanupOptions given = options;
        given.api_key = std::move(key);
        return std::make_unique<TranscriptCleanup>(
            discovery, keys.sources, [this] { return settings; }, id, model, reasoning, false,
            std::move(given));
    }

    std::shared_ptr<http::FakeHttpClient> provider;
    std::shared_ptr<http::FakeHttpClient> discovery = std::make_shared<http::FakeHttpClient>();
    TestKeys keys;
    Json settings = Json::object();
    CleanupOptions options;
};

} // namespace

TEST_CASE("cleanup preserves raw on truncation", "[test_text_generation]") {
    // from tests/test_text_generation.py::test_cleanup_preserves_raw_on_truncation
    for (const auto& [provider, model, protocol, path] : cases()) {
        INFO(provider << " " << model);
        forget_server_specs();
        Json raw = reply_for(protocol);
        if (protocol == "chat") {
            raw["choices"][0]["finish_reason"] = "length";
        } else if (protocol == "responses") {
            raw["status"] = "incomplete";
        } else if (protocol == "anthropic") {
            raw["stop_reason"] = "max_tokens";
        } else {
            raw["candidates"][0]["finishReason"] = "MAX_TOKENS";
        }
        auto fake = std::make_shared<http::FakeHttpClient>();
        fake->push(json_reply(200, raw));
        CleanupRig rig(fake);
        auto cleanup = rig.make(provider, model);
        CHECK(cleanup->cleanup("raw transcript") == "raw transcript");
        CHECK(cleanup->last_error());
        CHECK(cleanup->last_error() != "not run");
    }
    forget_server_specs();
}

TEST_CASE("canceled response cannot publish tools", "[test_text_generation]") {
    // from tests/test_text_generation.py::test_canceled_response_cannot_publish_tools
    for (const auto& [provider, model, protocol, path] : cases()) {
        INFO(provider << " " << model);
        const auto profile = *get_profile(provider, Json::object());
        CancelSource canceled;
        auto http = std::make_shared<CancelingClient>(canceled, reply_for(protocol, true));
        OpenAiCompatClient client(http, profile.base_url.value_or(""), "test-secret", {}, 600, 0);
        GenerateRequest request{model, messages_of({{"user", "Budget"}}),
                                Json::array({tool_schema()}), canceled.token()};
        try {
            (void)generate(client, profile, request);
            FAIL("generate returned");
        } catch (const TextGenerationError& error) {
            CHECK(std::string(error.what()) == "canceled");
        }
    }
}

TEST_CASE("empty new provider model does not reuse previous model", "[test_text_generation]") {
    // from
    // tests/test_text_generation.py::test_empty_new_provider_model_does_not_reuse_previous_model
    CleanupRig rig(std::make_shared<http::FakeHttpClient>());
    auto cleaner = rig.make("openai", "gpt-test", {}, "test");
    cleaner->configure("ollama", "");
    CHECK(cleaner->model().empty());
    CHECK_FALSE(cleaner->is_available());
}

TEST_CASE("responses reasoning preference is translated", "[test_text_generation]") {
    // from tests/test_text_generation.py::test_responses_reasoning_preference_is_translated
    for (const std::string provider : {"openai", "opencode_go"}) {
        Provider server(provider, {reply_for("responses")});
        GenerateRequest request{"gpt-5.6-luna", messages_of({{"user", "Hi"}})};
        request.reasoning_level = "high";
        (void)generate(*server.client, server.profile, request);
        CHECK(server.bodies()[0]["reasoning"] == Json{{"effort", "high"}});
    }
}

TEST_CASE("non reasoning responses preserve cleanup temperature and json",
          "[test_text_generation]") {
    // from
    // tests/test_text_generation.py::test_non_reasoning_responses_preserve_cleanup_temperature_and_json
    for (const std::string model : {"gpt-4o-mini", "gpt-4.1-mini"}) {
        Provider server("openai", {reply_for("responses")});
        GenerateRequest request{model, messages_of({{"user", "JSON please"}})};
        request.reasoning_level = "high";
        request.options = Json{{"temperature", 0}, {"response_format", {{"type", "json_object"}}}};
        (void)generate(*server.client, server.profile, request);
        const Json body = server.bodies()[0];
        CHECK(body["temperature"] == 0);
        CHECK(body["store"] == false);
        CHECK(body["text"]["format"] == Json{{"type", "json_object"}});
        CHECK_FALSE(body.contains("response_format"));
        CHECK_FALSE(body.contains("reasoning"));
        CHECK_FALSE(body.contains("include"));
    }
}

TEST_CASE("reasoning responses request encrypted context and omit temperature",
          "[test_text_generation]") {
    // from
    // tests/test_text_generation.py::test_reasoning_responses_request_encrypted_context_and_omit_temperature
    for (const std::string model : {"o3-mini", "o4-mini", "gpt-5.6-luna"}) {
        Provider server("openai", {reply_for("responses")});
        GenerateRequest request{model, messages_of({{"user", "Hi"}})};
        request.reasoning_level = "high";
        request.options = Json{{"temperature", 0}};
        (void)generate(*server.client, server.profile, request);
        const Json body = server.bodies()[0];
        CHECK(body["include"] == Json::array({"reasoning.encrypted_content"}));
        CHECK(body["reasoning"] == Json{{"effort", "high"}});
        CHECK_FALSE(body.contains("temperature"));
    }
}

TEST_CASE("custom responses cleanup and saved meeting tools", "[test_text_generation]") {
    // from tests/test_text_generation.py::test_custom_responses_cleanup_and_saved_meeting_tools
    for (const auto& [model, reasoning] :
         std::vector<std::pair<std::string, bool>>{{"gpt-4.1-mini", false}, {"o4-mini", true}}) {
        INFO(model);
        Json settings = Json::object();
        const auto profile = upsert_custom_profile(settings, "Gateway", "https://gateway.test/v1",
                                                   "", std::nullopt, "responses");
        auto fake = std::make_shared<http::FakeHttpClient>();
        for (const auto& name : {"responses", "responses_tool", "responses"}) {
            fake->push(json_reply(200, reply_body(name)));
        }
        CleanupRig rig(fake);
        rig.settings = settings;
        auto cleaner = rig.make(profile.id, model, {}, std::nullopt);
        CHECK(cleaner->cleanup("raw transcript") == "Clean text.");
        const auto snapshot = snapshot_from_profile(profile, model);
        // Editing a live endpoint does not change a saved meeting's protocol.
        (void)upsert_custom_profile(settings, "Gateway", *profile.base_url, "", profile.id, "chat");
        const auto restored = snapshot_from_mapping(snapshot.to_dict())->to_profile();
        CHECK(model_spec(restored, model).protocol == "responses");
        OpenAiCompatClient client(fake, *profile.base_url, "dummy", {}, 600, 0);
        Json messages = messages_of({{"user", "Budget?"}});
        GenerateRequest request{model, messages, Json::array({tool_schema()})};
        const auto first = generate(client, restored, request);
        messages.push_back(first.assistant_message);
        messages.push_back(Json{{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", "hits"}});
        request.messages = messages;
        CHECK(generate(client, restored, request).text == "Clean text.");
        const auto requests = fake->requests();
        for (const auto& sent : requests) {
            CHECK(url_path(sent.url) == "/v1/responses");
        }
        const Json cleanup_body = request_body(requests.front());
        if (reasoning) {
            CHECK_FALSE(cleanup_body.contains("temperature"));
        } else {
            CHECK(cleanup_body["temperature"] == 0);
        }
        const Json tool_body = request_body(requests.back());
        CHECK(tool_body["input"][1]["encrypted_content"] == "signed-reasoning");
        CHECK(tool_body["input"].back()["call_id"] == "call_1");
        CHECK(tool_body["store"] == false);
    }
}

TEST_CASE("ollama catalog filters embeddings and snapshots capabilities",
          "[test_text_generation]") {
    // from
    // tests/test_text_generation.py::test_ollama_catalog_filters_embeddings_and_snapshots_capabilities
    forget_server_specs();
    const auto profile = *get_profile("ollama", Json::object());
    http::FakeHttpClient fake;
    fake.set_handler([](const http::Request& request) {
        if (url_path(request.url) == "/v1/models") {
            return json_reply(200, Json{{"data", Json::array({Json{{"id", "test-embedding"}},
                                                              Json{{"id", "test-completion"}}})}});
        }
        REQUIRE(url_path(request.url) == "/api/show");
        const auto model = request_body(request).at("model").get<std::string>();
        return json_reply(200, Json{{"capabilities", model.find("embedding") != std::string::npos
                                                         ? Json::array({"embedding"})
                                                         : Json::array({"completion"})},
                                    {"parameters", "num_ctx 8192"}});
    });
    TestKeys keys;
    CHECK(list_chat_models(fake, profile, "", "", {}, &keys.sources) ==
          std::vector<std::string>{"test-completion"});
    const auto saved = snapshot_from_profile(profile, "test-completion", fake, keys.sources);
    REQUIRE(saved.model_metadata);
    CHECK((*saved.model_metadata)["tools"] == false);
    CHECK((*saved.model_metadata)["context_window"] == 8192);
    CHECK((*saved.model_metadata)["reasoning_format"] == "");
    CHECK(fake.requests().size() == 3);
    forget_server_specs();
}

TEST_CASE("provider http failures preserve raw", "[test_text_generation]") {
    // from tests/test_text_generation.py::test_provider_http_failures_preserve_raw
    for (const int status : {401, 403, 429, 404, 503}) {
        INFO(status);
        auto fake = std::make_shared<http::FakeHttpClient>();
        fake->set_handler([status](const http::Request&) {
            return json_reply(status, Json{{"error", {{"message", "Synthetic failure"}}}});
        });
        CleanupRig rig(fake);
        auto cleaner = rig.make("groq", "llama-3.3-70b-versatile", {}, "synthetic");
        CHECK(cleaner->cleanup("Keep this transcript") == "Keep this transcript");
        CHECK(cleaner->last_error());
        CHECK(cleaner->last_error() != "not run");
    }
}

TEST_CASE("luna cleanup serializes presets custom prompts and reasoning",
          "[test_text_generation]") {
    // from
    // tests/test_text_generation.py::test_luna_cleanup_serializes_presets_custom_prompts_and_reasoning
    // The level presets are services.cleanup_prompts' text; any prompt must reach the request
    // as given, so stand-ins carry the level's name.
    const std::vector<std::pair<std::string, std::string>> levels{
        {"light", "Light preset."},
        {"medium", "Medium preset."},
        {"high", "High preset."},
        {"custom", "Format as a checklist."}};
    const std::vector<std::pair<std::string, std::string>> efforts{
        {"off", "none"}, {"low", "low"}, {"medium", "medium"}, {"high", "high"}};
    for (const auto& [level, prompt] : levels) {
        for (const auto& [reasoning, effort] : efforts) {
            INFO(level << " " << reasoning);
            auto fake = std::make_shared<http::FakeHttpClient>();
            fake->push(json_reply(200, reply_for("responses")));
            CleanupRig rig(fake);
            auto cleanup = rig.make("openai", {}, reasoning);
            CHECK(cleanup->cleanup("raw transcript", prompt) == "Clean text.");
            CHECK_FALSE(cleanup->last_error());
            const auto requests = fake->requests();
            REQUIRE(requests.size() == 1);
            const Json body = request_body(requests[0]);
            CHECK(url_path(requests[0].url) == "/v1/responses");
            CHECK(body["model"] == "gpt-6-luna");
            CHECK(body["reasoning"] == Json{{"effort", effort}});
            CHECK(body["input"] ==
                  Json::array({Json{{"role", "system"}, {"content", prompt}},
                               Json{{"role", "user"}, {"content", "raw transcript"}}}));
            CHECK(body["store"] == false);
            CHECK(body["max_output_tokens"] == 16384);
            for (const auto& key : {"temperature", "max_tokens", "reasoning_effort"}) {
                CHECK_FALSE(body.contains(key));
            }
        }
    }
}

TEST_CASE("luna cleanup failure keeps raw transcript", "[test_text_generation]") {
    // from tests/test_text_generation.py::test_luna_cleanup_failure_keeps_raw_transcript
    for (const std::string failure : {"empty", "refusal", "incomplete", "http", "timeout"}) {
        INFO(failure);
        auto fake = std::make_shared<http::FakeHttpClient>();
        fake->set_handler([failure](const http::Request&) {
            if (failure == "timeout") {
                http::ScriptedResponse reply;
                reply.error = http::Error{http::ErrorKind::Timeout, "timed out"};
                return reply;
            }
            if (failure == "http") {
                return json_reply(503, Json{{"error", {{"message", "Unavailable"}}}});
            }
            Json raw = reply_for("responses");
            if (failure == "empty") {
                raw["output"] = Json::array();
            } else if (failure == "refusal") {
                raw["output"].back()["content"] =
                    Json::array({Json{{"type", "refusal"}, {"refusal", "Cannot comply"}}});
            } else {
                raw["status"] = "incomplete";
            }
            return json_reply(200, raw);
        });
        CleanupRig rig(fake, std::string(kOpenAiDefaultBaseUrl));
        auto cleanup = rig.make("openai", {});
        CHECK(cleanup->cleanup("raw transcript") == "raw transcript");
        CHECK(cleanup->last_error());
        CHECK(fake->requests().size() == 1);
    }
}

TEST_CASE("custom responses luna off keeps existing endpoint behavior", "[test_text_generation]") {
    // from
    // tests/test_text_generation.py::test_custom_responses_luna_off_keeps_existing_endpoint_behavior
    TextLLMProfile profile;
    profile.id = "custom_test";
    profile.name = "Local endpoint";
    profile.kind = "custom";
    profile.base_url = "http://localhost:1234/v1";
    profile.protocol = "responses";
    CHECK_FALSE(model_spec(profile, "gpt-6-luna").thinking_levels);
    Provider server(profile, {reply_for("responses")}, "test");
    GenerateRequest request{"gpt-6-luna", messages_of({{"user", "raw"}})};
    request.reasoning_level = "off";
    (void)generate(*server.client, server.profile, request);
    CHECK_FALSE(server.bodies()[0].contains("reasoning"));
}

namespace {

// tests/test_local_text_servers.py _chat_body: the body generate sends for `model`.
Json chat_body(const TextLLMProfile& profile, const std::string& model, Json options,
               const std::string& reasoning_level = {}) {
    auto fake = std::make_shared<http::FakeHttpClient>();
    fake->push(json_reply(
        200, Json{{"choices",
                   Json::array({Json{{"message", {{"role", "assistant"}, {"content", "Clean."}}},
                                     {"finish_reason", "stop"}}})}}));
    OpenAiCompatClient client(fake, profile.base_url.value_or(""), "dummy", {}, 600, 0);
    GenerateRequest request{model, messages_of({{"user", "raw"}})};
    request.reasoning_level = reasoning_level;
    request.options = std::move(options);
    (void)generate(client, profile, request);
    return request_body(fake->requests().at(0));
}

} // namespace

TEST_CASE("local servers keep cleanup options and leave output to the server",
          "[test_local_text_servers]") {
    // from
    // tests/test_local_text_servers.py::test_local_servers_keep_cleanup_options_and_leave_output_to_the_server
    forget_server_specs();
    http::FakeHttpClient server;
    server.set_handler(fixture_server(load_fixture("vllm")));
    TestKeys keys;
    const auto profile = *get_profile("vllm", Json::object());
    (void)list_chat_models(server, profile, "", "", {}, &keys.sources);
    const Json off = chat_body(profile, "Qwen/Qwen3-8B", Json{{"temperature", 0}}, "off");
    CHECK(off["temperature"] == 0);
    CHECK(off["reasoning_effort"] == "none");
    CHECK_FALSE(off.contains("max_tokens"));
    const Json high = chat_body(profile, "Qwen/Qwen3-8B", Json{{"temperature", 0}}, "high");
    CHECK(high["reasoning_effort"] == "high");
    CHECK_FALSE(high.contains("temperature"));
    // A model the server never described gets no reasoning field at all.
    const Json unknown = chat_body(profile, "other", Json{{"temperature", 0}}, "off");
    CHECK(golden::equal(
        Json{{"model", "other"}, {"messages", messages_of({{"user", "raw"}})}, {"temperature", 0}},
        unknown));
    forget_server_specs();
}

TEST_CASE("hosted catalogs still set their output budget", "[test_local_text_servers]") {
    // from tests/test_local_text_servers.py::test_hosted_catalogs_still_set_their_output_budget
    const auto profile = *get_profile("groq", Json::object());
    const Json body = chat_body(profile, "llama-3.3-70b-versatile", Json{{"temperature", 0}});
    CHECK(body["max_tokens"] == model_spec(profile, "llama-3.3-70b-versatile").max_output_tokens);
    CHECK_FALSE(body.contains("temperature"));
}

TEST_CASE("first cleanup learns the server before its request", "[test_local_text_servers]") {
    // from
    // tests/test_local_text_servers.py::test_first_cleanup_learns_the_server_before_its_request
    forget_server_specs();
    auto routes = fixture_server(load_fixture("ollama"));
    auto chat = std::make_shared<std::vector<Json>>();
    auto mutex = std::make_shared<std::mutex>();
    auto server = std::make_shared<http::FakeHttpClient>();
    server->set_handler([=](const http::Request& request) {
        if (request.method == "POST" && url_path(request.url) == "/v1/chat/completions") {
            std::lock_guard lock(*mutex);
            chat->push_back(request_body(request));
            return json_reply(
                200,
                Json{{"choices",
                      Json::array({Json{{"message", {{"role", "assistant"}, {"content", "Clean."}}},
                                        {"finish_reason", "stop"}}})}});
        }
        return routes(request);
    });
    TestKeys keys;
    TranscriptCleanup cleaner(
        server, keys.sources, [] { return Json::object(); }, "ollama", "qwen3", "off", false);
    CHECK(cleaner.cleanup("raw transcript", "Tidy.") == "Clean.");
    std::lock_guard lock(*mutex);
    REQUIRE(chat->size() == 1);
    CHECK((*chat)[0]["reasoning_effort"] == "none");
    CHECK((*chat)[0]["temperature"] == 0);
    const auto requests = server->requests();
    CHECK(url_path(requests.front().url) == "/v1/models");
    forget_server_specs();
}

TEST_CASE("thinking off adds nothing where hosted models have no off switch",
          "[test_local_text_servers]") {
    // from
    // tests/test_local_text_servers.py::test_thinking_off_adds_nothing_where_hosted_models_have_no_off_switch
    for (const auto& [provider, model] :
         std::vector<std::pair<std::string, std::string>>{{"openrouter", "vendor/small-model"},
                                                          {"groq", "llama-3.3-70b-versatile"},
                                                          {"opencode_go", "glm-5.2"}}) {
        INFO(provider);
        const Json body = chat_body(*get_profile(provider, Json::object()), model,
                                    Json{{"temperature", 0}}, "off");
        for (const auto& key : {"reasoning_effort", "reasoning", "thinking"}) {
            CHECK_FALSE(body.contains(key));
        }
    }
}

TEST_CASE("hosted endpoints are never asked about their models", "[test_local_text_servers]") {
    // from
    // tests/test_local_text_servers.py::test_hosted_endpoints_are_never_asked_about_their_models
    http::FakeHttpClient server;
    learn_server_models(server, *get_profile("groq", Json::object()), "key", nullptr);
    Json settings = Json::object();
    const auto hosted = upsert_custom_profile(settings, "Gateway", "https://gateway.test/v1");
    learn_server_models(server, hosted, "key", nullptr);
    CHECK(server.requests().empty());
}
