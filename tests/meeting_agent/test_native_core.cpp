#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "../llm/llm_test_support.h"
#include "../llm/sse_provider.h"
#include "base/testing/fake_clock.h"
#include "meeting/meeting_intelligence.h"
#include "meeting_agent/agent_request.h"
#include "meeting_agent/factory.h"
#include "meeting_agent/installed_core.h"
#include "meeting_agent/native_tools.h"
#include "meeting_agent/testing/fake_tool_host.h"
#include "meeting_agent/tool_policy.h"
#include "meeting_agent/tool_specs.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "process/testing/fake_launcher.h"

namespace {

using namespace std::chrono_literals;
using fv::Json;
using namespace fv::meeting_agent;
using fv::testing::SseProvider;
using fv::testing::SseReply;

std::shared_ptr<fv::http::Client> direct_client() {
    fv::http::CurlClientOptions options;
    options.proxy = [](std::string_view) {
        return std::optional<std::string>();
    };
    return fv::http::make_curl_client(std::move(options));
}

Json call(std::string id, std::string name, Json args, int index = 0) {
    return {{"index", index},
            {"id", std::move(id)},
            {"type", "function"},
            {"function", {{"name", std::move(name)}, {"arguments", args.dump()}}}};
}

Json choice(Json delta, Json finish = nullptr) {
    return {{"choices", Json::array({{{"index", 0},
                                      {"delta", std::move(delta)},
                                      {"finish_reason", std::move(finish)}}})}};
}

SseReply turn(Json delta, std::string finish = "stop") {
    return SseReply::stream(
        {SseReply::frame(choice(std::move(delta))), SseReply::frame(choice(Json::object(), finish)),
         SseReply::frame(
             {{"choices", Json::array()},
              {"usage", {{"prompt_tokens", 4}, {"completion_tokens", 2}, {"total_tokens", 6}}}}),
         "data: [DONE]\n\n"});
}

SseReply probe() {
    return SseReply::json(
        200,
        {{"choices",
          Json::array({{{"message",
                         {{"role", "assistant"},
                          {"tool_calls", Json::array({call("test", "noop", Json::object())})}}},
                        {"finish_reason", "tool_calls"}}})}});
}

Json patch(std::string text) {
    return {{"ops", Json::array({{{"op", "set_topic"},
                                  {"text", std::move(text)},
                                  {"evidence", Json::array({"sg_1"})}}})}};
}

fv::llm::TextLLMProfile profile(const SseProvider& provider) {
    return {"custom_local", "Local test", "custom", provider.url("/v1"), {}, false, {}, "chat"};
}

AgentConfig config(const SseProvider& provider) {
    AgentConfig config;
    config.provider = "custom_local";
    config.model = "local-model";
    config.system_prompt = "The meeting charter.";
    config.endpoint = fv::llm::snapshot_from_profile(profile(provider)).to_dict();
    return config;
}

SseReply http_error(int status) {
    auto reply = SseReply::json(status, {{"error", "private response body"}});
    reply.headers.emplace_back("Retry-After-Ms", "1");
    return reply;
}

bool await_request(const SseProvider& provider) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (provider.requests().empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    return !provider.requests().empty();
}

TEST_CASE("native meeting passes replay tool responses and start with fresh history", "[native]") {
    const Json toolCalls = Json::array({call("lookup", "search_past_meetings", {{"query", "plan"}}),
                                        call("write", "patch_state", patch("Review"), 1)});
    SseProvider provider(
        {probe(), turn({{"tool_calls", toolCalls}}, "tool_calls"), turn({{"content", "Ready."}}),
         turn({{"tool_calls", Json::array({call("second", "patch_state", patch("Next"))})}},
              "tool_calls"),
         turn({{"content", "Done."}})});
    fv::llm::test::TestKeys keys;
    CoreDeps deps{direct_client(), keys.sources};
    auto core = create_agent_core("pi", deps);
    auto tools = std::make_shared<testing::FakeToolHost>();
    tools->set_past_meetings_answer(
        {{"ok", true}, {"text", "Earlier plan"}, {"hits", Json::array()}});
    core->initialize(config(provider), tools);
    std::vector<std::string> observedTools;
    core->set_activity_callback([&](const AgentActivity& event) {
        if (event.kind == "tool") {
            observedTools.push_back(event.tool);
        }
    });
    const auto first = core->checkpoint(CheckpointPayload{.request_id = "first"});
    INFO(first.error);
    REQUIRE(first.ok);
    REQUIRE(first.op_results.size() == 1);
    CHECK(first.op_results.front().ok);
    CHECK(first.usage["total_tokens"] == 12);
    CHECK(tools->search_calls().size() == 1);
    const auto second = core->checkpoint(CheckpointPayload{.request_id = "second"});
    INFO(second.error);
    REQUIRE(second.ok);
    REQUIRE(second.op_results.size() == 1);
    CHECK(tools->ops().size() == 2);
    CHECK(std::ranges::find(observedTools, "search_past_meetings") != observedTools.end());

    const auto requests = provider.requests();
    REQUIRE(requests.size() == 5);
    CHECK(requests[0].json()["tools"][0]["function"]["name"] == "noop");
    const auto opening = requests[1].json();
    CHECK(opening["tools"] == native_tools());
    CHECK(opening["stream"] == true);
    CHECK(opening["messages"][0] == Json{{"role", "system"}, {"content", "The meeting charter."}});
    REQUIRE(opening["messages"].size() == 2);
    const auto replay = requests[2].json()["messages"];
    REQUIRE(replay.size() == 5);
    CHECK(replay[3]["tool_call_id"] == "lookup");
    CHECK(replay[3]["content"] == "Earlier plan");
    CHECK(replay[4]["tool_call_id"] == "write");
    CHECK(replay[4]["content"].get<std::string>().starts_with("1 applied, 0 rejected\n"));
    CHECK(requests[3].json()["messages"] == opening["messages"]);
    core->shutdown();
    CHECK_FALSE(core->is_healthy());
}

TEST_CASE("native agent cancellation preserves real writes and closes its tool authority",
          "[native]") {
    const auto duplicate =
        turn({{"tool_calls", Json::array({call("one", "patch_state", patch("First")),
                                          call("two", "patch_state", patch("Late"), 1)})}},
             "tool_calls");
    SseProvider provider({probe(), duplicate, turn({{"content", "Clean pass."}})});
    fv::llm::test::TestKeys keys;
    CoreDeps deps{direct_client(), keys.sources};
    auto core = create_agent_core("opencode", deps);
    auto tools = std::make_shared<testing::FakeToolHost>();
    core->initialize(config(provider), tools);
    tools->on_apply([&](const Json&) {
        core->cancel();
        return std::vector{fv::meeting_state::OpResult{.ok = true}};
    });
    const auto stopped = core->checkpoint(CheckpointPayload{});
    CHECK_FALSE(stopped.ok);
    CHECK(stopped.error == "canceled");
    // The second write never reached the host; its one op counts rejected.
    REQUIRE(stopped.op_results.size() == 2);
    CHECK(stopped.op_results.front().ok);
    CHECK_FALSE(stopped.op_results.back().ok);
    CHECK(tools->ops().size() == 1);
    CHECK(core->is_healthy());
    CHECK(core->checkpoint(CheckpointPayload{}).ok);
}

TEST_CASE("native notes passes reject non-note writes before reaching the host", "[native]") {
    const auto request =
        turn({{"tool_calls", Json::array({call("write", "patch_state", patch("Review"))})}},
             "tool_calls");
    SseProvider provider({probe(), request, turn({{"content", "Understood."}})});
    fv::llm::test::TestKeys keys;
    CoreDeps deps{direct_client(), keys.sources};
    auto core = create_agent_core("direct", deps);
    auto tools = std::make_shared<testing::FakeToolHost>();
    core->initialize(config(provider), tools);
    const auto answer = core->checkpoint(CheckpointPayload{.is_notes = true});
    REQUIRE(answer.ok);
    REQUIRE(answer.op_results.size() == 1);
    CHECK(answer.op_results.front().reason == NOTES_ONLY);
    CHECK(tools->ops().empty());
    CHECK(provider.requests()[1].json()["messages"][0]["content"] != "The meeting charter.");
}

TEST_CASE("native loop retries bounded transient errors and never echoes provider bodies",
          "[native]") {
    for (const auto status : {400, 401, 429, 503}) {
        CAPTURE(status);
        const bool recovers = status == 429;
        const int calls = recovers ? 3 : status == 503 ? 4 : 1;
        std::vector<SseReply> replies(static_cast<std::size_t>(calls), http_error(status));
        if (recovers) {
            replies.back() = turn({{"content", "Ready."}});
        }
        SseProvider provider(std::move(replies));
        const auto endpoint = profile(provider);
        AgentLoop loop(direct_client(), endpoint, "local-model",
                       fv::llm::model_spec(endpoint, "local-model"), {}, "dummy",
                       fv::system_clock());
        const auto outcome = loop.run_pass("charter", "prompt", meeting_tools(), {}, {5, 2}, {});
        CHECK(outcome.ok == recovers);
        CHECK_FALSE(outcome.canceled);
        if (!recovers) {
            // ARCHITECTURE.md's provider error; the body names no message, so nothing follows.
            CHECK(outcome.error ==
                  "Local test model turn failed (HTTP " + std::to_string(status) + ")");
            CHECK(outcome.error.find("private") == std::string::npos);
        }
        CHECK(provider.requests().size() == static_cast<std::size_t>(calls));
    }
}

TEST_CASE("native loop cancels a silent stream at the stall budget", "[native]") {
    auto silent = SseReply::stream({});
    silent.stall_after = 0;
    SseProvider provider({silent, turn({{"content", "Done."}})});
    const auto endpoint = profile(provider);
    fv::FakeClock clock;
    AgentLoop loop(direct_client(), endpoint, "local-model",
                   fv::llm::model_spec(endpoint, "local-model"), {}, "dummy", clock);
    fv::CancelSource cancel;
    auto pass = std::async(std::launch::async, [&] {
        return loop.run_pass("charter", "prompt", meeting_tools(), {}, {60, 1}, cancel.token());
    });
    const bool started = await_request(provider);
    clock.advance(2s);
    const bool finished = pass.wait_for(2s) == std::future_status::ready;
    if (!started || !finished) {
        cancel.cancel();
    }
    const auto outcome = pass.get();
    REQUIRE(started);
    REQUIRE(finished);
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.error == "Agent pass stalled after 1s without agent progress");
    CHECK(loop.run_pass("charter", "new prompt", meeting_tools(), {}, {60, 1}, {}).ok);
}

TEST_CASE("meeting agent factory routes built-in and installed kinds explicitly", "[native]") {
    fv::llm::test::TestKeys keys;
    fv::net::Reactor reactor;
    CoreDeps deps{
        direct_client(), keys.sources, {}, &reactor, std::make_shared<fv::FakeProcessLauncher>()};
    for (std::string_view kind : {"pi", "opencode", "direct"}) {
        CHECK(dynamic_cast<NativeAgentCore*>(create_agent_core(kind, deps).get()) != nullptr);
    }
    for (std::string_view kind : {"codex", "claude_code", "opencode_cli"}) {
        CHECK(dynamic_cast<InstalledAgentCore*>(create_agent_core(kind, deps).get()) != nullptr);
    }
    CHECK_THROWS_WITH(create_agent_core("unknown", deps),
                      "Unsupported meeting agent core: 'unknown'");
}

TEST_CASE(
    "stored intelligence needs a grant and runs through native tools on the recorded endpoint",
    "[native]") {
    SseProvider provider(
        {probe(),
         turn({{"tool_calls", Json::array({call("write", "patch_state", patch("Review"))})}},
              "tool_calls"),
         turn({{"content", "Done."}})});
    fv::llm::test::TestKeys keys;
    CoreDeps deps{direct_client(), keys.sources};
    bool granted = false;
    const auto endpoint = config(provider);
    fv::meeting::MeetingIntelligence intelligence(
        [&](std::string_view kind) { return create_agent_core(kind, deps); }, {},
        [&](const MeetingAgentRoute& route) {
            return granted && route.endpoint == endpoint.endpoint;
        });
    fv::meeting_state::MeetingState state;
    state.meeting_id = "m_test";
    state.status = "ended";
    state.cloud_enabled = true;
    fv::meeting::RerunContext context;
    context.meeting_id = state.meeting_id;
    context.store = std::make_shared<fv::meeting_state::MeetingStateStore>(std::move(state));
    context.repository = std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>();
    context.meeting = {{"agent_provider", endpoint.provider},
                       {"agent_model", endpoint.model},
                       {"agent_endpoint_json", endpoint.endpoint.dump()}};
    context.segments = {
        {{"id", "sg_1"}, {"text", "Planning the review"}, {"start_s", 0.0}, {"end_s", 3.0}}};
    CHECK(intelligence.consolidate(context, {})["status"] == "unavailable");
    CHECK(provider.requests().empty());
    granted = true;
    const auto result = intelligence.consolidate(context, {});
    INFO(result.dump());
    CHECK(result["status"] == "completed");
    CHECK(result["applied"] == 1);
    CHECK(provider.requests().size() == 3);
    CHECK(context.store->snapshot().dump().find("Review") != std::string::npos);
}

} // namespace
