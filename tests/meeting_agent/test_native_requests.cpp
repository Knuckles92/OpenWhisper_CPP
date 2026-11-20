// What the built-in agent asks each provider for (test_text_generation.py's sidecar and direct
// agent tests, ported against the native loop): the endpoint fields a pass runs on, a read
// tool's result going back through each wire protocol, and the tool-capability probe.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <tuple>
#include <vector>

#include "base/identity.h"
#include "llm/profiles.h"
#include "meeting_agent/agent_request.h"
#include "native_support.h"

namespace {

using namespace fv::meeting_agent;
using namespace fv::meeting_agent::native_test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;

struct Case {
    std::string provider, model, protocol, prefix;
};

// test_text_generation.py's CASES: the provider, the model, its wire protocol, and the path the
// profile's base URL ends in (the loopback provider stands in for the host).
const std::vector<Case> kCases = {
    {"openai", "gpt-6-luna", "responses", "/v1"},
    {"openai", "o4-mini", "responses", "/v1"},
    {"openai", "gpt-5.6-luna", "responses", "/v1"},
    {"openai", "gpt-4o-mini", "responses", "/v1"},
    {"openai", "gpt-4.1-mini", "responses", "/v1"},
    {"openai", "gpt-3.5-turbo", "chat", "/v1"},
    {"ollama", "llama3.2", "chat", "/v1"},
    {"vllm", "qwen3-8b", "chat", "/v1"},
    {"groq", "llama-3.3-70b-versatile", "chat", "/openai/v1"},
    {"opencode_go", "glm-5.2", "chat", "/zen/go/v1"},
    {"opencode_go", "gpt-5.6-luna", "responses", "/zen/go/v1"},
    {"opencode_go", "minimax-m3", "anthropic", "/zen/go/v1"},
    {"opencode_zen", "glm-5.2", "chat", "/zen/v1"},
    {"opencode_zen", "gpt-5.6-luna", "responses", "/zen/v1"},
    {"opencode_zen", "claude-sonnet-4-6", "anthropic", "/zen/v1"},
    {"opencode_zen", "gemini-3.1-pro", "google", "/zen/v1"},
};

bool new_provider(const Case& entry) {
    return entry.provider != "openai";
}

// test_text_generation.py's reply(): a whole (non-streamed) answer in `protocol`, calling
// `tool` with {"query": "budget"} when it is not empty.
Json reply(const std::string& protocol, const std::string& tool = {}) {
    const bool calls = !tool.empty();
    const Json args = tool == "noop" ? Json::object() : Json{{"query", "budget"}};
    if (protocol == "chat") {
        Json message = {{"role", "assistant"},
                        {"content", calls ? "" : "Clean text."},
                        {"reasoning_content", "private reasoning"}};
        if (calls) {
            message["tool_calls"] =
                Json::array({{{"id", "call_1"},
                              {"type", "function"},
                              {"function", {{"name", tool}, {"arguments", args.dump()}}}}});
        }
        return {{"choices", Json::array({{{"message", message},
                                          {"finish_reason", calls ? "tool_calls" : "stop"}}})}};
    }
    if (protocol == "responses") {
        Json output = Json::array({{{"type", "reasoning"},
                                    {"id", "rs_1"},
                                    {"summary", Json::array()},
                                    {"encrypted_content", "signed-reasoning"}}});
        if (calls) {
            output.push_back({{"type", "function_call"},
                              {"id", "fc_1"},
                              {"call_id", "call_1"},
                              {"name", tool},
                              {"arguments", args.dump()}});
        } else {
            output.push_back({{"type", "message"},
                              {"id", "msg_1"},
                              {"role", "assistant"},
                              {"status", "completed"},
                              {"content", Json::array({{{"type", "output_text"},
                                                        {"text", "Clean text."},
                                                        {"annotations", Json::array()}}})}});
        }
        return {{"id", "resp_1"}, {"status", "completed"}, {"output", output}};
    }
    if (protocol == "anthropic") {
        Json content = Json::array({{{"type", "thinking"},
                                     {"thinking", "private reasoning"},
                                     {"signature", "signed-reasoning"}}});
        if (calls) {
            content.push_back(
                {{"type", "tool_use"}, {"id", "call_1"}, {"name", tool}, {"input", args}});
        } else {
            content.push_back({{"type", "text"}, {"text", "Clean text."}});
        }
        return {{"content", content}, {"stop_reason", calls ? "tool_use" : "end_turn"}};
    }
    Json parts = Json::array({{{"text", "private reasoning"},
                               {"thought", true},
                               {"thoughtSignature", "signed-reasoning"}}});
    if (calls) {
        parts.push_back({{"functionCall", {{"id", "call_1"}, {"name", tool}, {"args", args}}},
                         {"thoughtSignature", "tool-signature"}});
    } else {
        parts.push_back({{"text", "Clean text."}});
    }
    return {{"candidates", Json::array({{{"content", {{"role", "model"}, {"parts", parts}}},
                                         {"finishReason", "STOP"}}})}};
}

fv::llm::TextLLMProfile builtin(const std::string& id) {
    return *fv::llm::get_profile(id, Json::object());
}

AgentConfig config_for(const Case& entry, const SseProvider* provider = nullptr) {
    AgentConfig config;
    config.meeting_id = "meeting-1";
    config.provider = entry.provider;
    config.model = entry.model;
    config.api_key = "test-secret";
    config.system_prompt = "Use evidence.";
    auto endpoint = fv::llm::snapshot_from_profile(builtin(entry.provider), entry.model).to_dict();
    if (provider != nullptr) {
        endpoint["base_url"] = provider->url(entry.prefix);
    }
    config.endpoint = std::move(endpoint);
    return config;
}

TEST_CASE("native OpenAI models run on the Responses API", "[native]") {
    for (const std::string model : {"gpt-4o-mini", "gpt-4.1-mini", "o4-mini"}) {
        CAPTURE(model);
        AgentConfig config;
        config.meeting_id = "meeting-1";
        config.provider = "openai";
        config.model = model;
        config.system_prompt = "Use evidence.";
        const auto fields = endpoint_fields(config);
        CHECK(fields["model_metadata"]["protocol"] == "responses");
        CHECK(fields["model_metadata"]["reasoning"] == model.starts_with("o4"));
    }
}

TEST_CASE("an OpenAI meeting's stale saved metadata takes the current route", "[native]") {
    for (const auto& [model, protocol, reasoning] :
         {std::tuple{"o4-mini", "responses", true},
          std::tuple{"gpt-4.1-mini", "responses", false}}) {
        CAPTURE(model);
        const auto snapshot = fv::llm::snapshot_from_mapping(
            {{"profile_id", "openai"},
             {"name", "OpenAI"},
             {"kind", "openai"},
             {"base_url", nullptr},
             {"api_key_env", "OPENAI_API_KEY"},
             {"model_metadata", {{"model", model}, {"protocol", "chat"}, {"reasoning", true}}}});
        REQUIRE(snapshot.has_value());
        const auto spec = fv::llm::model_spec(snapshot->to_profile(), model);
        CHECK(spec.protocol == protocol);
        CHECK(spec.reasoning == reasoning);
    }
}

TEST_CASE("a meeting's endpoint fields carry its protocol and headers but never its key",
          "[native]") {
    for (const auto& entry : kCases) {
        if (!new_provider(entry)) {
            continue;
        }
        CAPTURE(entry.provider, entry.model);
        const auto fields = endpoint_fields(config_for(entry));
        CHECK(fields["model_metadata"]["protocol"] == entry.protocol);
        CHECK(fields.dump().find("test-secret") == std::string::npos);
        if (entry.provider == "opencode_go") {
            CHECK(fields["headers"]["x-opencode-session"] == "meeting-1");
        }
    }
}

TEST_CASE("a read tool's result goes back to the model through each wire protocol", "[native]") {
    const auto entry = GENERATE(from_range(kCases));
    CAPTURE(entry.provider, entry.model, entry.protocol);
    const auto profile = builtin(entry.provider);
    std::vector<SseReply> replies;
    if (!profile.refuses_unsupported_tools()) {
        replies.push_back(SseReply::json(200, reply(entry.protocol, "noop")));
    }
    const std::size_t probes = replies.size();
    replies.push_back(SseReply::json(200, reply(entry.protocol, "search_context_files")));
    replies.push_back(SseReply::json(200, reply(entry.protocol)));
    SseProvider provider(std::move(replies));
    fv::llm::test::TestKeys keys;
    CoreDeps deps{direct_client(), keys.sources};
    auto core = create_agent_core("pi", deps);
    auto tools = std::make_shared<testing::FakeToolHost>();
    tools->set_context_files_answer({{"hits", Json::array()}, {"text", "No matching evidence."}});
    core->initialize(config_for(entry, &provider), tools);
    const auto result = core->checkpoint({.request_id = "req-1"});
    INFO(result.error);
    REQUIRE(result.ok);
    const auto calls = tools->search_calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].tool == "search_context_files");
    CHECK(calls[0].query == "budget");
    CHECK(calls[0].target.empty());
    CHECK(calls[0].limit == 10);

    const auto requests = provider.requests();
    REQUIRE(requests.size() == probes + 2);
    for (const auto& request : requests) {
        CHECK(request.path.starts_with(entry.prefix + "/"));
    }
    const auto replay = requests.back().body;
    CHECK_THAT(replay, ContainsSubstring("call_1"));
    CHECK_THAT(replay, ContainsSubstring("No matching evidence."));
    if (entry.protocol != "chat") {
        CHECK_THAT(replay, ContainsSubstring("signed-reasoning"));
    }
}

TEST_CASE("an endpoint that might ignore tools must call the test tool", "[native]") {
    // test_direct_agent_probe_needs_a_tool_call_unless_the_api_rejects_tools (the probe half)
    // and test_pi_has_a_custom_localhost_model_call_the_test_tool. A custom endpoint, local or
    // not, is probed with Pi's request; OpenRouter refuses tools it cannot use and is not.
    for (const std::string host : {"127.0.0.1", "localhost"}) {
        CAPTURE(host);
        SseProvider provider({SseReply::json(200, reply("chat"))});
        std::string url = provider.url("/v1");
        url.replace(url.find("127.0.0.1"), 9, host);
        Json settings = Json::object();
        const auto box = fv::llm::upsert_custom_profile(settings, "Box", url);
        fv::llm::test::TestKeys keys;
        CoreDeps deps{direct_client(), keys.sources};
        auto core = create_agent_core("pi", deps);
        AgentConfig config;
        config.meeting_id = "meeting-1";
        config.provider = box.id;
        config.model = "qwen3";
        config.system_prompt = "Use evidence.";
        config.endpoint = fv::llm::snapshot_from_profile(box, "qwen3").to_dict();
        CHECK_THROWS_WITH(core->initialize(config, std::make_shared<testing::FakeToolHost>()),
                          "Model did not call the test tool. Choose a tool-capable model for " +
                              std::string(fv::identity().product) + ".");
        const auto requests = provider.requests();
        REQUIRE(requests.size() == 1);
        const auto body = requests[0].json();
        CHECK(body["tools"] ==
              Json::array(
                  {{{"type", "function"},
                    {"function",
                     {{"name", "noop"},
                      {"description", "Verify tool support."},
                      {"parameters", {{"type", "object"}, {"properties", Json::object()}}}}}}}));
        CHECK(body["max_tokens"] == 512);
        CHECK(body["messages"] ==
              Json::array(
                  {{{"role", "user"}, {"content", "Call the noop tool with no arguments."}}}));
    }

    SseProvider provider({});
    fv::llm::test::TestKeys keys;
    CoreDeps deps{direct_client(), keys.sources};
    auto core = create_agent_core("pi", deps);
    AgentConfig config;
    config.meeting_id = "meeting-1";
    config.provider = "openrouter";
    config.model = "qwen3";
    config.api_key = "key";
    config.system_prompt = "Use evidence.";
    auto endpoint = fv::llm::snapshot_from_profile(builtin("openrouter"), "qwen3").to_dict();
    endpoint["base_url"] = provider.url("/api/v1");
    config.endpoint = endpoint;
    core->initialize(config, std::make_shared<testing::FakeToolHost>());
    CHECK(core->is_healthy());
    CHECK(provider.requests().empty());
}

} // namespace
