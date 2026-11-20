// Live agent work during silence and around human input (tests/test_meeting_live_agent_workflow.py,
// the scheduler traces; the two built-in agent tests are M8.8's).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "agents/loopback_mcp.h"
#include "base/testing/fake_clock.h"
#include "meeting_agent/prompts.h"
#include "meeting_agent/scheduler.h"
#include "scheduler_support.h"

using fv::Json;
using namespace fv::meeting_agent;
using namespace fv::meeting_agent::scheduler_test;
using namespace std::chrono_literals;
using P = SchedulerProbe;

namespace {

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

std::vector<Json> spaced(int count) {
    std::vector<Json> rows;
    for (int i = 0; i < count; ++i) {
        rows.push_back(segment("sg_" + std::to_string(i), i * 200.0));
    }
    return rows;
}

} // namespace

// from
// tests/test_meeting_live_agent_workflow.py::test_guidance_revisits_consumed_notes_without_new_speech
TEST_CASE("test_guidance_revisits_consumed_notes_without_new_speech",
          "[test_meeting_live_agent_workflow]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeNotesAgent agent;
    auto host = notes_host({segment("sg_old", 1, "Anthropic makes Claude.")});
    CheckpointScheduler scheduler(*host, agent.core, clock);
    P::fire(scheduler);
    const auto sent = P::notes_sent_starts(scheduler);
    CHECK(sent == std::map<std::string, double, std::less<>>{{"sg_old", 1.0}});
    scheduler.notify_guidance();
    P::fire(scheduler);
    std::vector<CheckpointPayload> reviews;
    for (const auto& payload : agent.calls()) {
        if (payload.is_notes && payload.state_snapshot.value("notes_review_requested", false)) {
            reviews.push_back(payload);
        }
    }
    REQUIRE(reviews.size() == 1);
    CHECK(reviews[0].new_segments[0]["text"] == "Anthropic makes Claude.");
    CHECK(contains(build_notes_user_prompt(reviews[0].state_snapshot, reviews[0].new_segments),
                   "No new speech is required"));
}

// from tests/test_meeting_live_agent_workflow.py::test_first_cleanup_runs_during_silence
TEST_CASE("test_first_cleanup_runs_during_silence", "[test_meeting_live_agent_workflow]") {
    fv::agents::Event cleaned;
    FakeNotesAgent agent;
    agent.core->set_hook([&](const CheckpointPayload& payload, AgentToolHost*) {
        if (payload.is_polish) {
            cleaned.set();
        }
        return ok_with("add_item");
    });
    SchedulerOptions options;
    options.tick_s = 0.01;
    options.polish_initial_delay_s = 0.02;
    options.min_interval_s = 0.01;
    auto host = notes_host({segment("sg_1", 1)});
    CheckpointScheduler scheduler(*host, agent.core, fv::system_clock(), {}, {}, options);
    scheduler.start();
    scheduler.notify_segments(1);
    // A short meeting must not need six checkpoints to get cleanup.
    CHECK(cleaned.wait(2s));
    std::size_t cards = 0;
    for (const auto& p : agent.calls()) {
        cards += (p.is_polish || p.is_notes) ? 0 : 1;
    }
    CHECK(cards == 1);
    std::this_thread::sleep_for(40ms);
    std::size_t polishes = 0;
    for (const auto& p : agent.calls()) {
        polishes += p.is_polish ? 1 : 0;
    }
    CHECK(polishes == 1);
    scheduler.stop();
}

// from
// tests/test_meeting_live_agent_workflow.py::test_user_request_precedes_follow_on_background_passes
TEST_CASE("test_user_request_precedes_follow_on_background_passes",
          "[test_meeting_live_agent_workflow]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeNotesAgent agent;
    auto host = notes_host({segment("sg_1", 1)});
    CheckpointScheduler scheduler(*host, agent.core, clock);
    P::set_successful_checkpoints(scheduler, 5);
    std::future<AgentResult> request;
    agent.core->set_hook([&](const CheckpointPayload& payload, AgentToolHost*) {
        if (!payload.is_notes) {
            request = P::push_note_request(scheduler, "Use bullets");
        }
        return ok_with("add_item");
    });
    P::fire(scheduler);
    CHECK(agent.calls().size() == 1); // neither notes nor polish jump ahead
    P::fire_note_request(scheduler);
    REQUIRE(request.valid());
    CHECK(request.get().ok);
    REQUIRE(agent.calls().size() == 2);
    CHECK(agent.calls()[1].state_snapshot["note_adjustment_request"] == "Use bullets");
}

// from
// tests/test_meeting_live_agent_workflow.py::test_later_speech_can_correct_a_previously_consumed_name
TEST_CASE("test_later_speech_can_correct_a_previously_consumed_name",
          "[test_meeting_live_agent_workflow]") {
    fv::FakeClock clock;
    start_late(clock);
    auto host = notes_host({segment("sg_old", 1, "We are evaluating Entropic.")});
    FakeNotesAgent agent;
    CheckpointScheduler scheduler(*host, agent.core, clock);
    P::fire(scheduler);
    host->add_segments({segment("sg_new", 5, "Their Claude model is the candidate.")});
    P::fire(scheduler);
    const auto payload = agent.calls().back();
    CHECK(ids_of(payload.new_segments) == std::vector<std::string>{"sg_new"});
    const auto prompt = build_checkpoint_user_prompt(payload.state_snapshot, payload.new_segments);
    CHECK(contains(prompt, "[sg_old]"));
    CHECK(contains(prompt, "evaluating Entropic"));
    CHECK(contains(prompt, "[sg_new]"));
    CHECK(contains(prompt, "Claude"));
}

// from
// tests/test_meeting_live_agent_workflow.py::test_polish_changes_trigger_dashboard_and_notes_review
TEST_CASE("test_polish_changes_trigger_dashboard_and_notes_review",
          "[test_meeting_live_agent_workflow]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeNotesAgent agent;
    agent.core->set_hook([](const CheckpointPayload&, AgentToolHost*) {
        AgentResult result;
        result.ok = true;
        fv::meeting_state::OpResult op;
        op.ok = true;
        op.op = Json{{"op", "revise_segment_text"}, {"segment_id", "sg_1"}, {"text", "Anthropic"}};
        result.op_results.push_back(op);
        return result;
    });
    auto host = notes_host({segment("sg_1", 1)});
    CheckpointScheduler scheduler(*host, agent.core, clock);
    P::set_successful_checkpoints(scheduler, 6);
    P::maybe_fire_polish(scheduler);
    CHECK(P::guidance_pending(scheduler));
    CHECK(P::notes_guidance_pending(scheduler));
}

// from
// tests/test_meeting_live_agent_workflow.py::test_failed_notes_retry_in_silence_and_do_not_spin
TEST_CASE("test_failed_notes_retry_in_silence_and_do_not_spin",
          "[test_meeting_live_agent_workflow]") {
    fv::agents::Event finished;
    std::mutex mutex;
    std::vector<std::chrono::steady_clock::time_point> attempts;
    FakeNotesAgent agent;
    agent.core->set_hook([&](const CheckpointPayload& payload, AgentToolHost*) {
        if (payload.is_notes) {
            std::size_t count = 0;
            {
                std::lock_guard lock(mutex);
                attempts.push_back(std::chrono::steady_clock::now());
                count = attempts.size();
            }
            if (count == 1) {
                return failed("temporary");
            }
            finished.set();
        }
        AgentResult ok;
        ok.ok = true;
        return ok;
    });
    SchedulerOptions options;
    options.tick_s = 0.01;
    options.notes_min_interval_s = 0.08;
    options.min_interval_s = 0.01;
    auto host = notes_host({segment("sg_1", 1)});
    CheckpointScheduler scheduler(*host, agent.core, fv::system_clock(), {}, {}, options);
    scheduler.start();
    scheduler.notify_segments(1);
    CHECK(finished.wait(2s));
    scheduler.stop();
    std::lock_guard lock(mutex);
    REQUIRE(attempts.size() == 2);
    CHECK(attempts[1] - attempts[0] >= 80ms);
}

// from
// tests/test_meeting_live_agent_workflow.py::test_notes_backlog_keeps_earliest_unprocessed_segments
TEST_CASE("test_notes_backlog_keeps_earliest_unprocessed_segments",
          "[test_meeting_live_agent_workflow]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeNotesAgent agent;
    auto host = notes_host(spaced(5));
    SchedulerOptions options;
    options.notes_max_segments = 2;
    CheckpointScheduler scheduler(*host, agent.core, clock, {}, {}, options);
    P::set_successful_checkpoints(scheduler, 1);
    P::maybe_fire_notes(scheduler);
    REQUIRE(agent.calls().size() == 1);
    CHECK(ids_of(agent.calls()[0].new_segments) == std::vector<std::string>{"sg_0", "sg_1"});
    P::set_successful_checkpoints(scheduler, 3);
    P::maybe_fire_notes(scheduler);
    REQUIRE(agent.calls().size() == 2);
    CHECK(ids_of(agent.calls()[1].new_segments) == std::vector<std::string>{"sg_2", "sg_3"});
}

// from
// tests/test_meeting_live_agent_workflow.py::test_guidance_review_does_not_skip_unprocessed_note_backlog
TEST_CASE("test_guidance_review_does_not_skip_unprocessed_note_backlog",
          "[test_meeting_live_agent_workflow]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeNotesAgent agent;
    auto host = notes_host(spaced(5));
    SchedulerOptions options;
    options.notes_max_segments = 2;
    CheckpointScheduler scheduler(*host, agent.core, clock, {}, {}, options);
    P::set_notes_guidance_pending(scheduler, true);
    P::maybe_fire_notes(scheduler);
    REQUIRE(agent.calls().size() == 1);
    CHECK(ids_of(agent.calls()[0].new_segments) == std::vector<std::string>{"sg_3", "sg_4"});
    CHECK(P::notes_sent_starts(scheduler).empty());
    P::set_successful_checkpoints(scheduler, 2);
    P::maybe_fire_notes(scheduler);
    REQUIRE(agent.calls().size() == 2);
    CHECK(ids_of(agent.calls()[1].new_segments) == std::vector<std::string>{"sg_0", "sg_1"});
}

// from
// tests/test_meeting_live_agent_workflow.py::test_new_human_guidance_gets_an_attempt_despite_earlier_failure_backoff
TEST_CASE("test_new_human_guidance_gets_an_attempt_despite_earlier_failure_backoff",
          "[test_meeting_live_agent_workflow]") {
    fv::FakeClock clock;
    start_late(clock);
    auto host = notes_host({});
    FakeNotesAgent agent;
    CheckpointScheduler scheduler(*host, agent.core, clock);
    P::set_retry_not_before(scheduler, P::mono(scheduler) + 300);
    P::set_notes_retry_not_before(scheduler, P::mono(scheduler) + 45);
    scheduler.notify_guidance();
    CHECK(P::retry_not_before(scheduler) == 0.0);
    CHECK(P::notes_retry_not_before(scheduler) == 0.0);
}
