#pragma once

// Shared pieces of the native agent core's loopback tests (test_native_core_*.cpp): a curl
// client that never uses a proxy, Chat Completions stream frames, the tool-capability probe's
// answer, and a core over an SseProvider with a FakeToolHost. Nothing leaves 127.0.0.1.

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../llm/llm_test_support.h"
#include "../llm/sse_provider.h"
#include "base/testing/fake_clock.h"
#include "http/client.h"
#include "meeting_agent/factory.h"
#include "meeting_agent/testing/fake_tool_host.h"
#include "meeting_agent/tool_specs.h"

namespace fv::meeting_agent::native_test {

using namespace std::chrono_literals;
using fv::testing::SseProvider;
using fv::testing::SseReply;

inline std::shared_ptr<http::Client> direct_client() {
    http::CurlClientOptions options;
    options.proxy = [](std::string_view) {
        return std::optional<std::string>();
    };
    return http::make_curl_client(std::move(options));
}

inline Json call(std::string id, std::string name, Json args, int index = 0) {
    return {{"index", index},
            {"id", std::move(id)},
            {"type", "function"},
            {"function", {{"name", std::move(name)}, {"arguments", args.dump()}}}};
}

inline Json choice(Json delta, Json finish = nullptr) {
    return {{"choices", Json::array({{{"index", 0},
                                      {"delta", std::move(delta)},
                                      {"finish_reason", std::move(finish)}}})}};
}

inline std::string usage_frame() {
    return SseReply::frame(
        {{"choices", Json::array()},
         {"usage", {{"prompt_tokens", 4}, {"completion_tokens", 2}, {"total_tokens", 6}}}});
}

// One streamed model turn: `delta` in one frame, then the finish reason and the usage.
inline SseReply turn(Json delta, std::string finish = "stop") {
    return SseReply::stream({SseReply::frame(choice(std::move(delta))),
                             SseReply::frame(choice(Json::object(), finish)), usage_frame(),
                             "data: [DONE]\n\n"});
}

// A turn that calls these tools, in order.
inline SseReply tools_turn(std::vector<Json> calls) {
    Json list = Json::array();
    int index = 0;
    for (auto& entry : calls) {
        entry["index"] = index++;
        list.push_back(std::move(entry));
    }
    return turn({{"tool_calls", std::move(list)}}, "tool_calls");
}

inline SseReply text_turn(std::string text = "Done.") {
    return turn({{"content", std::move(text)}});
}

// A turn that thinks in `pieces` reasoning frames before it answers.
inline SseReply thinking_turn(int pieces, std::string text = "Done.") {
    std::vector<std::string> frames;
    for (int i = 0; i < pieces; ++i) {
        frames.push_back(SseReply::frame(choice({{"reasoning_content", "step "}})));
    }
    frames.push_back(SseReply::frame(choice({{"content", std::move(text)}})));
    frames.push_back(SseReply::frame(choice(Json::object(), "stop")));
    frames.push_back(usage_frame());
    frames.push_back("data: [DONE]\n\n");
    return SseReply::stream(std::move(frames));
}

// A stream that answers its head and then says nothing.
inline SseReply silent() {
    auto reply = SseReply::stream({});
    reply.stall_after = 0;
    return reply;
}

// The answer to initialize's "Call the noop tool" probe.
inline SseReply probe() {
    return SseReply::json(
        200,
        {{"choices",
          Json::array({{{"message",
                         {{"role", "assistant"},
                          {"tool_calls", Json::array({call("test", "noop", Json::object())})}}},
                        {"finish_reason", "tool_calls"}}})}});
}

inline Json patch(Json ops) {
    return {{"ops", std::move(ops)}};
}

inline Json set_topic(std::string text) {
    return {{"op", "set_topic"}, {"text", std::move(text)}, {"evidence", Json::array({"sg_1"})}};
}

inline llm::TextLLMProfile local_profile(const SseProvider& provider) {
    return {"custom_local", "Local test", "custom", provider.url("/v1"), {}, false, {}, "chat"};
}

inline AgentConfig local_config(const SseProvider& provider) {
    AgentConfig config;
    config.meeting_id = "m_side";
    config.provider = "custom_local";
    config.model = "local-model";
    config.system_prompt = "The meeting charter.";
    config.endpoint = llm::snapshot_from_profile(local_profile(provider)).to_dict();
    return config;
}

// A built-in or installed core over `provider`, initialized with a fresh FakeToolHost.
struct Harness {
    explicit Harness(std::vector<SseReply> replies, std::string_view kind = "pi",
                     const Clock* clock = nullptr)
        : provider(std::move(replies)) {
        deps.http = direct_client();
        deps.clock = clock;
        core = create_agent_core(kind, deps);
        core->initialize(local_config(provider), tools);
    }

    // The JSON bodies the provider received, the probe first.
    std::vector<Json> bodies() const {
        std::vector<Json> out;
        for (const auto& request : provider.requests()) {
            out.push_back(request.json());
        }
        return out;
    }

    SseProvider provider;
    llm::test::TestKeys keys;
    CoreDeps deps{nullptr, keys.sources};
    std::shared_ptr<testing::FakeToolHost> tools = std::make_shared<testing::FakeToolHost>();
    std::unique_ptr<AgentCore> core;
};

// Waits (real time) until the provider has received `count` requests.
inline bool await_requests(const SseProvider& provider, std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (provider.requests().size() < count && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    return provider.requests().size() >= count;
}

// Cancels `core` when the scope ends, so a failed REQUIRE cannot leave a pass blocked on a
// silent provider while its std::async future waits for it. Declare it after the future.
struct CancelOnExit {
    AgentCore& core;
    ~CancelOnExit() { core.cancel(); }
};

template <class Result>
bool finishes(std::future<Result>& pending, std::chrono::milliseconds wait) {
    return pending.wait_for(wait) == std::future_status::ready;
}

} // namespace fv::meeting_agent::native_test
