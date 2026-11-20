// from tests/test_meeting_finalization.py

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/cancel.h"
#include "meeting/finalization.h"

using fv::Json;
using namespace fv::meeting;
using namespace std::chrono_literals;

namespace {

Json with_detail(Json step, std::string detail) {
    step["detail"] = std::move(detail);
    return step;
}

constexpr std::chrono::year_month_day kBeforeShutdown{std::chrono::year{2026},
                                                      std::chrono::month{11}, std::chrono::day{16}};

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// AgentResult(ok=...): the result type the meeting passes return.
struct AgentResult {
    bool ok = false;
    std::string error;
};

} // namespace

TEST_CASE("failure summary preserves reason and multiple failed stages",
          "[test_meeting_finalization]") {
    // from
    // tests/test_meeting_finalization.py::test_failure_summary_preserves_reason_and_multiple_failed_stages
    const Json polish = with_detail(make_step("polish", "failed"), "Timed out after 60s.");
    const Json summary = with_detail(make_step("consolidation", "failed"), "Provider unavailable.");
    const std::string result =
        failed_steps_message(Json::array({polish, summary, make_step("finalize", "completed")}));
    CHECK(contains(result, "Transcript Cleanup, Summary & Action Items failed"));
    CHECK(contains(result, "Timed out after 60s."));
    CHECK(contains(result, "Provider unavailable."));
    CHECK(contains(result, "recording and transcript were kept"));
    CHECK_FALSE(contains(result, "State Finalization:"));
}

TEST_CASE("legacy failure without detail and success", "[test_meeting_finalization]") {
    // from tests/test_meeting_finalization.py::test_legacy_failure_without_detail_and_success
    CHECK(contains(failed_steps_message(Json::array({{{"id", "polish"}, {"status", "failed"}}})),
                   "polish failed"));
    CHECK(failed_steps_message(Json::array({make_step("polish", "completed")})).empty());
}

TEST_CASE("ready messages name only what was found", "[test_meeting_finalization]") {
    // from tests/test_meeting_finalization.py::test_ready_messages_name_only_what_was_found
    const Json stats = {
        {"segments", 32}, {"words", 410}, {"key_points", 4}, {"action_items", 0}, {"decisions", 1}};
    CHECK(insights_ready_message(stats) ==
          "Final insights ready — 32 segments, 4 key points, 1 decisions.");
    CHECK(saved_state_detail(stats) == "Saved 32 segments (410 words)");
}

TEST_CASE("only openai is offered", "[test_meeting_finalization]") {
    // from tests/test_meeting_finalization.py::TestSpeakerPassGate::test_only_openai_is_offered
    const std::string backend = GENERATE("off", "local", "", "bogus");
    const SpeakerPassGate gate =
        speaker_pass_gate(backend, true, [] { return true; }, kBeforeShutdown);
    CHECK_FALSE(gate.ok);
    CHECK_FALSE(gate.offered);
    CHECK(gate.reason == "Speaker identification is not set to OpenAI.");
}

TEST_CASE("retired model is refused even with consent", "[test_meeting_finalization]") {
    // from
    // tests/test_meeting_finalization.py::TestSpeakerPassGate::test_retired_model_is_refused_even_with_consent
    const SpeakerPassGate gate = speaker_pass_gate(
        "openai", true, [] { return true; },
        std::chrono::year_month_day{std::chrono::year{2027}, std::chrono::month{2},
                                    std::chrono::day{26}});
    CHECK_FALSE(gate.ok);
    CHECK_FALSE(gate.offered);
    CHECK(contains(gate.reason, "retired"));
}

TEST_CASE("consent is checked before the key is read", "[test_meeting_finalization]") {
    // from
    // tests/test_meeting_finalization.py::TestSpeakerPassGate::test_consent_is_checked_before_the_key_is_read
    int reads = 0;
    const SpeakerPassGate gate = speaker_pass_gate(
        "openai", false,
        [&] {
            ++reads;
            return true;
        },
        kBeforeShutdown);
    CHECK(reads == 0);
    CHECK_FALSE(gate.ok);
    CHECK(gate.offered);
    CHECK(gate.reason == "Audio-upload consent has not been given.");
}

TEST_CASE("missing key is refused", "[test_meeting_finalization]") {
    // from tests/test_meeting_finalization.py::TestSpeakerPassGate::test_missing_key_is_refused
    // Python's two parameters ("" and None) are both "no key" here; a lookup that throws is the
    // third way to find none.
    const bool throws = GENERATE(false, true);
    const SpeakerPassGate gate = speaker_pass_gate(
        "openai", true,
        [&]() -> bool {
            if (throws) {
                throw std::runtime_error("keyring unavailable");
            }
            return false;
        },
        kBeforeShutdown);
    CHECK_FALSE(gate.ok);
    CHECK(gate.offered);
    CHECK(gate.reason == "No OpenAI API key is configured.");
}

TEST_CASE("eligible gate carries the key without printing it", "[test_meeting_finalization]") {
    // from
    // tests/test_meeting_finalization.py::TestSpeakerPassGate::test_eligible_gate_carries_the_key_without_printing_it
    // The gate never receives the key: it only learns that one exists, so nothing in it can
    // print the key.
    int reads = 0;
    const SpeakerPassGate gate = speaker_pass_gate(
        "openai", true,
        [&] {
            ++reads;
            return true;
        },
        kBeforeShutdown);
    CHECK(reads == 1);
    CHECK(gate == SpeakerPassGate{true, "", true});
}

TEST_CASE("injected decoder needs no key", "[test_meeting_finalization]") {
    // from
    // tests/test_meeting_finalization.py::TestSpeakerPassGate::test_injected_decoder_needs_no_key
    const SpeakerPassGate gate = speaker_pass_gate("openai", true, {}, kBeforeShutdown);
    CHECK(gate.ok);
    CHECK(gate.reason.empty());
}

TEST_CASE("returns the result", "[test_meeting_finalization]") {
    // from tests/test_meeting_finalization.py::TestRunAgentCall::test_returns_the_result
    const auto result = run_agent_call<AgentResult>(
        [](const fv::CancelToken&) { return AgentResult{true, ""}; }, 5s, "test-call");
    REQUIRE(result.has_value());
    CHECK(result->ok);
}

TEST_CASE("a raising call is a failed result", "[test_meeting_finalization]") {
    // from
    // tests/test_meeting_finalization.py::TestRunAgentCall::test_a_raising_call_is_a_failed_result
    const auto result = run_agent_call<AgentResult>(
        [](const fv::CancelToken&) -> AgentResult { throw std::runtime_error("sidecar died"); }, 5s,
        "test-call");
    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(result.error().timed_out);
    CHECK(result.error().message == "sidecar died");
}

TEST_CASE("timeout runs the handler before cancel", "[test_meeting_finalization]") {
    // from
    // tests/test_meeting_finalization.py::TestRunAgentCall::test_timeout_runs_the_handler_before_cancel
    struct Order {
        std::mutex mutex;
        std::vector<std::string> events;
        void add(std::string event) {
            const std::lock_guard lock(mutex);
            events.push_back(std::move(event));
        }
    };
    auto order = std::make_shared<Order>();
    const auto result = run_agent_call<AgentResult>(
        [order](const fv::CancelToken& cancel) {
            // The cancel is the release: the call unwinds as soon as it is cancelled.
            if (cancel.wait_for(10s)) {
                order->add("cancel");
            }
            return AgentResult{true, ""};
        },
        50ms, "test-call", [order] { order->add("revoke"); });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().timed_out);
    const std::lock_guard lock(order->mutex);
    CHECK(order->events == std::vector<std::string>{"revoke", "cancel"});
}
