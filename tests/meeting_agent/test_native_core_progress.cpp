// What a native pass reports while it runs (test_meeting_sidecar.py's progress notifications
// and TestAgentActivity): the finalization card's sentences, unchanged by the activity strip,
// in the running pass's wording, and the strip's ticks with the real tool names and pass kind.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "meeting_agent/progress_labels.h"
#include "native_support.h"

namespace {

using namespace fv::meeting_agent;
using namespace fv::meeting_agent::native_test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;

constexpr std::string_view kThinking = "Model is thinking through the transcript…";

struct Recorder {
    void attach(AgentCore& core) {
        core.set_progress_callback([this](const std::string& line) {
            std::lock_guard lock(mutex);
            progress.push_back(line);
        });
        core.set_activity_callback([this](const AgentActivity& tick) {
            std::lock_guard lock(mutex);
            activity.push_back(tick);
        });
    }
    std::mutex mutex;
    std::vector<std::string> progress;
    std::vector<AgentActivity> activity;
};

TEST_CASE("thinking events read as thinking whatever their delta", "[native]") {
    for (const std::string_view delta : {"thinking_delta", "thinking_start"}) {
        for (const std::string_view pass : {"", "cards", "notes", "polish", "consolidation"}) {
            CHECK(progress_detail("message_update", delta, pass) == kThinking);
        }
        CHECK(activity_kind("message_update", delta) == "thinking");
    }
}

TEST_CASE("a cards pass reports in a rolling pass's words, never the final report's", "[native]") {
    Harness harness({probe(), thinking_turn(2)});
    Recorder recorder;
    recorder.attach(*harness.core);
    REQUIRE(harness.core->checkpoint({.request_id = "req-cards"}).ok);
    REQUIRE_FALSE(recorder.progress.empty());
    CHECK(recorder.progress.front() == "Reviewing the last few minutes…");
    CHECK(recorder.progress.back() == "Checkpoint is wrapping up…");
    CHECK(std::ranges::find(recorder.progress, kThinking) != recorder.progress.end());
    for (const auto& line : recorder.progress) {
        CHECK_THAT(line, !ContainsSubstring("final report"));
    }
    REQUIRE_FALSE(recorder.activity.empty());
    const auto& start = recorder.activity.front();
    CHECK(start.kind == "start");
    CHECK(start.label == "Reviewing the last few minutes…");
    CHECK(start.tool.empty());
    CHECK(start.pass_kind == "cards");
}

TEST_CASE("every event reaches the progress callback, repeated sentences included", "[native]") {
    // sidecar.py called the callback once per notification; an activity sink beside it does
    // not change the card's copy. A tool's start and end both read "Updating the dashboard…".
    Harness harness({probe(),
                     tools_turn({call("w", "patch_state", patch(Json::array({set_topic("A")})))}),
                     text_turn()});
    Recorder recorder;
    recorder.attach(*harness.core);
    REQUIRE(harness.core->checkpoint({.request_id = "req-1"}).ok);
    const auto& lines = recorder.progress;
    const auto first = std::ranges::find(lines, "Updating the dashboard…");
    REQUIRE(first != lines.end());
    REQUIRE(std::next(first) != lines.end());
    CHECK(*std::next(first) == "Updating the dashboard…");
    CHECK(recorder.activity.size() == lines.size());
    for (std::size_t i = 0; i < lines.size() && i < recorder.activity.size(); ++i) {
        CHECK(recorder.activity[i].label == lines[i]);
    }
}

TEST_CASE("activity ticks carry the thinking, the real tool names and the pass kind", "[native]") {
    Harness harness(
        {probe(),
         SseReply::stream(
             {SseReply::frame(choice({{"reasoning_content", "Who decided?"}})),
              SseReply::frame(choice(
                  {{"tool_calls",
                    Json::array(
                        {call("p", "patch_state", patch(Json::array({set_topic("A")}))),
                         call("q", "ask_question", {{"text", "Who?"}}, 1),
                         call("r", "resolve_question",
                              {{"question_id", "q1"}, {"answer_text", "Ana"}, {"confidence", 0.5}},
                              2)})}})),
              SseReply::frame(choice(Json::object(), "tool_calls")), usage_frame(),
              "data: [DONE]\n\n"}),
         text_turn()});
    Recorder recorder;
    recorder.attach(*harness.core);
    REQUIRE(harness.core->consolidate({.request_id = "req-consol", .is_consolidation = true}).ok);

    const auto thinking = std::ranges::find_if(
        recorder.activity, [](const AgentActivity& tick) { return tick.kind == "thinking"; });
    REQUIRE(thinking != recorder.activity.end());
    CHECK(thinking->label == kThinking);
    CHECK(thinking->pass_kind == "consolidation");
    CHECK_THAT(thinking->ts, ContainsSubstring("+00:00"));

    std::vector<std::string> tools;
    for (const auto& tick : recorder.activity) {
        CHECK(tick.pass_kind == "consolidation");
        if (tick.label == "Updating the dashboard…") {
            CHECK(tick.kind == "tool");
            if (tools.empty() || tools.back() != tick.tool) {
                tools.push_back(tick.tool);
            }
        }
    }
    CHECK(tools == std::vector<std::string>{"patch_state", "ask_question", "resolve_question"});
}

TEST_CASE("a progress or activity sink that throws cannot fail the pass", "[native]") {
    Harness harness({probe(), text_turn()});
    harness.core->set_progress_callback([](const std::string&) { throw std::runtime_error("x"); });
    harness.core->set_activity_callback(
        [](const AgentActivity&) { throw std::runtime_error("y"); });
    CHECK(harness.core->checkpoint({.request_id = "req-1"}).ok);
}

} // namespace
