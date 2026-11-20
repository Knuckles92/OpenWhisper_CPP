// The native meeting agent's loop (M8's AgentLoop::run_pass) against a scripted OpenAI-compatible
// provider on 127.0.0.1: every response's first byte waits 200 ms, then the stream gives 50
// tokens 10 ms apart. Two kinds of pass, 20 each:
//
// text      one turn of 50 tokens.
// tools     a turn that streams two tool calls (answered at once by the executor), then a turn of
//           50 tokens.
//
// agent_ttft_overhead_p95_ms is the time from run_pass to the first text delta minus the
// provider's 200 ms; agent_pass_overhead_p95_ms is a pass's time minus everything the provider
// was scripted to spend (first-byte waits and token gaps), over both kinds. OpenWhisper's number
// would come from its Pi sidecar against the same provider (lead decision 5); without it these
// budgets are absolute.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../http/loopback_server.h"
#include "base/clock.h"
#include "http/client.h"
#include "llm/model_catalog.h"
#include "meeting_agent/agent_loop.h"
#include "meeting_agent/tool_specs.h"
#include "pyport/json.h"
#include "support/perf_support.h"

namespace {

using namespace std::chrono_literals;
using fv::Json;
using fv::testing::LoopbackServer;

constexpr auto kFirstByte = 200ms;
constexpr auto kTokenGap = 10ms;
constexpr int kTokens = 50;
constexpr int kPasses = 20;

std::string frame(const Json& data) {
    return "data: " + fv::py::dumps(data) + "\n\n";
}

Json choice(Json delta, Json finish = nullptr) {
    return {{"choices", Json::array({{{"index", 0},
                                      {"delta", std::move(delta)},
                                      {"finish_reason", std::move(finish)}}})}};
}

struct Script {
    std::vector<std::string> frames;
    // Before each frame after the first.
    std::chrono::milliseconds gap{0};
};

Script tokens() {
    Script script;
    script.gap = kTokenGap;
    for (int index = 0; index < kTokens; ++index) {
        script.frames.push_back(frame(choice({{"content", "tok" + std::to_string(index) + " "}})));
    }
    script.frames.push_back(frame(choice(Json::object(), "stop")));
    script.frames.push_back(frame({{"choices", Json::array()},
                                   {"usage",
                                    {{"prompt_tokens", 40},
                                     {"completion_tokens", kTokens},
                                     {"total_tokens", 40 + kTokens}}}}));
    script.frames.push_back("data: [DONE]\n\n");
    return script;
}

Script toolCalls() {
    const auto call = [](std::string id, int index) {
        return Json{
            {"index", index},
            {"id", std::move(id)},
            {"type", "function"},
            {"function",
             {{"name", "search_past_meetings"}, {"arguments", Json{{"query", "plan"}}.dump()}}}};
    };
    Script script;
    script.frames.push_back(
        frame(choice({{"tool_calls", Json::array({call("one", 0), call("two", 1)})}})));
    script.frames.push_back(frame(choice(Json::object(), "tool_calls")));
    script.frames.push_back("data: [DONE]\n\n");
    return script;
}

// Serves the scripted streams in order, each after the first-byte wait.
class Provider {
public:
    Provider()
        : server_([this](const LoopbackServer::Request&, LoopbackServer::Reply& reply) {
              serve(reply);
          }) {}

    void push(Script script) {
        std::lock_guard lock(mutex_);
        scripts_.push_back(std::move(script));
    }
    std::string url(std::string_view path) const { return server_.url(path); }

private:
    void serve(LoopbackServer::Reply& reply) {
        std::optional<Script> script;
        {
            std::lock_guard lock(mutex_);
            if (!scripts_.empty()) {
                script = std::move(scripts_.front());
                scripts_.pop_front();
            }
        }
        if (!script) {
            reply.write(LoopbackServer::response(404, "Not Found", {}, "{}"));
            return;
        }
        if (!reply.wait(kFirstByte)) {
            return;
        }
        reply.write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: "
                    "no-cache\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        for (std::size_t index = 0; index < script->frames.size(); ++index) {
            if (index > 0 && script->gap.count() > 0 && !reply.wait(script->gap)) {
                return;
            }
            const std::string& piece = script->frames[index];
            char size[24];
            std::snprintf(size, sizeof size, "%zx\r\n", piece.size());
            reply.write(std::string(size) + piece + "\r\n");
        }
        reply.write("0\r\n\r\n");
    }

    std::mutex mutex_;
    std::deque<Script> scripts_;
    LoopbackServer server_; // last: stopped first
};

// The loop reports a text, thinking or tool-call delta at most once per 2 s of its clock after
// any event (a pass's "turn_start" included), so on a real clock the first token of a pass is
// never reported. This clock moves 3 s at every read: every event is reported, and the pass's
// budget, read from the same clock, is set far beyond what the reads add up to. The times this
// test reports are steady_clock's, never this clock's.
class JumpingClock final : public fv::Clock {
public:
    std::chrono::steady_clock::time_point steady() const override {
        return start_ + std::chrono::seconds(3) * (reads_.fetch_add(1) + 1);
    }
    std::chrono::system_clock::time_point utc() const override { return fv::system_clock().utc(); }
    const std::chrono::time_zone* zone() const override { return fv::system_clock().zone(); }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    mutable std::atomic<std::int64_t> reads_{0};
};

std::chrono::duration<double, std::milli> scripted(const Script& script) {
    return kFirstByte + script.gap * static_cast<int>(script.frames.size() - 1);
}

} // namespace

TEST_CASE("agent loop time to first token and pass overhead", "[perf]") {
    Provider provider;
    fv::http::CurlClientOptions options;
    options.proxy = [](std::string_view) {
        return std::optional<std::string>();
    };
    std::shared_ptr<fv::http::Client> http = fv::http::make_curl_client(std::move(options));
    const fv::llm::TextLLMProfile endpoint{
        "custom_local", "Local test", "custom", provider.url("/v1"), {}, false, {}, "chat"};
    const JumpingClock clock;
    fv::meeting_agent::AgentLoop loop(http, endpoint, "local-model",
                                      fv::llm::model_spec(endpoint, "local-model"), {}, "dummy",
                                      clock);
    int toolRuns = 0;
    const fv::meeting_agent::ToolExecutor tools = [&toolRuns](std::string_view, const Json&) {
        ++toolRuns;
        return std::string("Earlier plan");
    };

    std::vector<double> ttft, passOverhead;
    for (int pass = 0; pass < 2 * kPasses; ++pass) {
        const bool withTools = pass % 2 == 1;
        double scriptedMs = 0;
        if (withTools) {
            const Script calls = toolCalls();
            scriptedMs += scripted(calls).count();
            provider.push(calls);
        }
        const Script text = tokens();
        scriptedMs += scripted(text).count();
        provider.push(text);

        std::optional<fvperf::Clock::time_point> firstToken;
        const auto started = fvperf::Clock::now();
        const auto outcome = loop.run_pass(
            "The meeting charter.", "What changed?", fv::meeting_agent::meeting_tools(), tools,
            {1e12, std::nullopt}, {}, [&](const fv::meeting_agent::LoopEvent& event) {
                if (!firstToken && event.type == "message_update" && event.delta == "text_delta") {
                    firstToken = fvperf::Clock::now();
                }
            });
        const double passMs = fvperf::msSince(started);
        INFO(outcome.error);
        REQUIRE(outcome.ok);
        REQUIRE(firstToken.has_value());
        passOverhead.push_back(passMs - scriptedMs);
        if (!withTools) {
            ttft.push_back(
                std::chrono::duration<double, std::milli>(*firstToken - started).count() -
                std::chrono::duration<double, std::milli>(kFirstByte).count());
        }
    }
    CHECK(toolRuns == 2 * kPasses);

    const std::string test = "perf.agent loop time to first token and pass overhead";
    const Json notes{{"first_byte_ms", 200},
                     {"tokens", kTokens},
                     {"token_gap_ms", 10},
                     {"passes", 2 * kPasses},
                     {"tool_passes", kPasses}};
    const auto first = fvperf::record("agent_ttft_overhead_p95_ms", "ms", ttft, test, notes);
    CHECK(fvperf::underCeiling(first, first.p95));
    const auto pass = fvperf::record("agent_pass_overhead_p95_ms", "ms", passOverhead, test, notes);
    CHECK(fvperf::underCeiling(pass, pass.p95));
}
