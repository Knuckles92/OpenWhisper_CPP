// CheckpointScheduler: adaptive intervals, warm-up, coalescing, the sent-segment watermark,
// failure backoff and health, the end-of-meeting passes and the Jaccard early fire
// (tests/test_meeting_scheduler.py), with the note-taker cadence of
// tests/test_meeting_notes_agent.py and the guidance passes of tests/test_meeting_corrections.py.
// Direct calls run on a FakeClock; tests that drive the real worker use the system clock, as the
// Python tests use real time.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "agents/loopback_mcp.h"
#include "base/testing/fake_clock.h"
#include "meeting_agent/scheduler.h"
#include "meeting_agent/testing/fake_agent_core.h"
#include "meeting_agent/testing/fake_scheduler_host.h"
#include "pyport/text.h"
#include "scheduler_support.h"

using fv::Json;
using namespace fv::meeting_agent;
using namespace fv::meeting_agent::scheduler_test;
using namespace std::chrono_literals;
using P = SchedulerProbe;

namespace {

SchedulerOptions fast_intervals(double seconds = 0.05) {
    SchedulerOptions options;
    options.base_interval_s = seconds;
    options.min_interval_s = seconds;
    options.max_interval_s = seconds;
    return options;
}

std::vector<Json> numbered(int count, const std::string& text = "hello there") {
    std::vector<Json> rows;
    for (int i = 0; i < count; ++i) {
        rows.push_back(row("sg_" + std::to_string(i), i, i + 1, text));
    }
    return rows;
}

// The meeting-to-date transcript as a polish stand-in hands it to the agent: one block, through
// `agent.checkpoint` as a polish pass with a fresh snapshot (meeting/finalization.py's
// polish_transcript, whose blocks are the engine's and tested there).
struct PolishStandIn {
    std::vector<std::vector<Json>> seen;
    PolishTranscript fn() {
        return [this](AgentCore& agent, fv::meeting_state::MeetingStateStore& store,
                      const std::vector<Json>& segments, double,
                      const std::function<void(const std::string&, int, int)>&)
                   -> std::optional<std::string> {
            seen.push_back(segments);
            CheckpointPayload payload;
            payload.request_id = "polish-1";
            payload.state_snapshot = store.snapshot();
            payload.new_segments = segments;
            payload.is_polish = true;
            const auto result = agent.checkpoint(payload);
            if (!result.ok) {
                return result.error;
            }
            return std::nullopt;
        };
    }
};

} // namespace

// ---- TestAdaptiveInterval ----

// from
// tests/test_meeting_scheduler.py::TestAdaptiveInterval::test_defaults_prioritize_live_dashboard_freshness
TEST_CASE("test_defaults_prioritize_live_dashboard_freshness", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    FakeSchedulerHost host;
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock);
    CHECK(P::interval_for(sched, 2) == 20.0);
    CHECK(P::interval_for(sched, 3) == 15.0);
    CHECK(P::interval_for(sched, 8) == 5.0);
}

// from tests/test_meeting_scheduler.py::TestAdaptiveInterval::test_pinned_intervals
TEST_CASE("test_pinned_intervals", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    FakeSchedulerHost host;
    FakeAgent agent;
    SchedulerOptions options;
    options.base_interval_s = 45.0;
    options.min_interval_s = 30.0;
    options.max_interval_s = 60.0;
    CheckpointScheduler sched(host, agent.core, clock, {}, {}, options);
    CHECK(P::interval_for(sched, 0) == 60.0); // quiet (< 3)
    CHECK(P::interval_for(sched, 2) == 60.0);
    CHECK(P::interval_for(sched, 3) == 45.0); // mid
    CHECK(P::interval_for(sched, 7) == 45.0);
    CHECK(P::interval_for(sched, 8) == 30.0); // pressure floor
    CHECK(P::interval_for(sched, 20) == 30.0);
}

// ---- TestInitialContext ----

// from
// tests/test_meeting_scheduler.py::TestInitialContext::test_waits_until_two_minutes_then_sends_accumulated_context
TEST_CASE("test_waits_until_two_minutes_then_sends_accumulated_context",
          "[test_meeting_scheduler]") {
    const bool guidance = GENERATE(false, true);
    INFO("guidance " << guidance);
    std::vector<Json> segments;
    for (int i = 0; i < 12; ++i) {
        segments.push_back(
            row("sg_" + std::to_string(i), i, i, "Opening claim " + std::to_string(i)));
    }
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host(segments, {.clock_s = 119.9});
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock);
    sched.notify_segments(static_cast<int>(segments.size()));
    if (guidance) {
        sched.notify_guidance();
    }
    const int pending = P::pending_segments(sched);
    const auto seq_before = host.store()->seq();

    sched.run_due();
    CHECK(agent.calls().empty());
    CHECK(host.store()->seq() == seq_before);
    CHECK(P::pending_segments(sched) == pending);
    CHECK(P::guidance_pending(sched) == guidance);
    CHECK(P::sent_starts(sched).empty());

    host.set_clock_s(120.0);
    sched.run_due();
    REQUIRE_FALSE(agent.calls().empty());
    CHECK(ids_of(agent.calls()[0].new_segments) == ids_of(segments));
    CHECK_FALSE(agent.notes_calls().empty());
}

// from
// tests/test_meeting_scheduler.py::TestInitialContext::test_background_notes_and_polish_wait_even_without_pending_speech
TEST_CASE("test_background_notes_and_polish_wait_even_without_pending_speech",
          "[test_meeting_scheduler]") {
    // Python patches the notes and polish passes to see they are not called; here both are due
    // (six successful checkpoints, transcript present), so a call would show as a pass.
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host({segment("sg_1", 1.0)}, {.clock_s = 30.0});
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock);
    P::set_successful_checkpoints(sched, 6);
    sched.run_due();
    CHECK(agent.core->passes().empty());

    host.set_clock_s(200.0);
    sched.run_due();
    CHECK_FALSE(agent.core->passes().empty());
}

// from
// tests/test_meeting_scheduler.py::TestInitialContext::test_paused_time_does_not_complete_warmup
TEST_CASE("test_paused_time_does_not_complete_warmup", "[test_meeting_scheduler]") {
    // The host's meeting clock excludes paused time: 60 s of meeting stays 60 s however long
    // the pause, and 60 s more of meeting after it completes the warm-up.
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host({row("sg_1", 1.0, 3.0, "Opening")}, {.clock_s = 0.0});
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock);
    sched.notify_segments(1);
    host.set_clock_s(60.0);
    clock.advance(540s); // paused
    sched.run_due();
    CHECK(agent.calls().empty());
    host.set_clock_s(120.0);
    clock.advance(60s);
    sched.run_due();
    CHECK(card_passes(agent.calls()).size() == 1);
}

// from
// tests/test_meeting_scheduler.py::TestInitialContext::test_short_meeting_still_gets_final_insights
TEST_CASE("test_short_meeting_still_gets_final_insights", "[test_meeting_scheduler]") {
    const Json seg = row("sg_1", 1.0, 3.0, "We agreed to ship.");
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host({seg}, {.clock_s = 30.0});
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock);
    const auto result = sched.run_consolidation();
    CHECK(result.status == "completed");
    REQUIRE(agent.calls().size() == 1);
    CHECK(agent.calls()[0].is_consolidation);
    CHECK(agent.calls()[0].new_segments == std::vector<Json>{seg});
}

// ---- TestCoalescing ----

// from
// tests/test_meeting_scheduler.py::TestCoalescing::test_pending_while_in_flight_coalesce_into_next_fire
TEST_CASE("test_pending_while_in_flight_coalesce_into_next_fire", "[test_meeting_scheduler]") {
    FakeSchedulerHost host({row("sg_1", 1.0, 2.0, "one"), row("sg_2", 3.0, 4.0, "two")});
    FakeAgent agent;
    agent.blocking = true;
    agent.released->clear();
    CheckpointScheduler sched(host, agent.core, fv::system_clock(), {}, {}, fast_intervals());
    P::set_last_fire_mono(sched, P::mono(sched) - 1.0);
    sched.start();
    sched.notify_segments(2);
    CHECK(agent.entered.wait(2s));
    // New transcript lands while the first checkpoint is in flight; the single worker coalesces
    // it into the immediate follow-up fire.
    host.add_segments({row("sg_3", 5.0, 6.0, "three")});
    sched.notify_segments(1);
    agent.released->set();
    CHECK(eventually([&] { return agent.calls().size() >= 2; }));
    const auto calls = agent.calls();
    REQUIRE(calls.size() >= 2);
    const auto second = ids_of(calls[1].new_segments);
    CHECK(std::find(second.begin(), second.end(), "sg_3") != second.end());
    agent.released->set();
    sched.stop();
}

// ---- TestSegmentWatermark ----

// from
// tests/test_meeting_scheduler.py::TestSegmentWatermark::test_out_of_order_channel_arrival_loses_nothing
TEST_CASE("test_out_of_order_channel_arrival_loses_nothing", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    start_late(clock);
    // Mic chunk 0 (one long segment) is transcribed first.
    FakeSchedulerHost host({row("mic_1", 0.0, 30.0, "mic one")});
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock, {}, {}, fast_intervals());
    P::fire(sched);
    REQUIRE(agent.calls().size() == 1);
    CHECK(ids_of(agent.calls()[0].new_segments) == std::vector<std::string>{"mic_1"});

    // Loopback chunk 0 covers the same wall-clock window but lands after: every one of its
    // segments starts before the mic segment's end.
    host.add_segments({row("loop_1", 0.0, 8.0, "loop one"), row("loop_2", 8.0, 16.0, "loop two"),
                       row("loop_3", 16.0, 24.0, "loop three"),
                       row("loop_4", 24.0, 32.0, "loop four")});
    P::fire(sched);
    std::vector<CheckpointPayload> cards;
    for (const auto& call : agent.calls()) {
        if (!call.is_polish) {
            cards.push_back(call);
        }
    }
    REQUIRE(cards.size() == 2);
    CHECK(ids_of(cards[1].new_segments) ==
          std::vector<std::string>{"loop_1", "loop_2", "loop_3", "loop_4"});
}

// from tests/test_meeting_scheduler.py::TestSegmentWatermark::test_sent_segments_are_never_resent
TEST_CASE("test_sent_segments_are_never_resent", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host({row("sg_1", 0.0, 5.0, "one"), row("sg_2", 5.0, 10.0, "two")});
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock, {}, {}, fast_intervals());
    P::fire(sched);
    REQUIRE(agent.calls().size() == 1);
    CHECK(ids_of(agent.calls()[0].new_segments) == std::vector<std::string>{"sg_1", "sg_2"});
    // Nothing new arrived: the re-fetch window returns the same rows, all filtered out, so no
    // checkpoint fires at all.
    P::fire(sched);
    CHECK(agent.calls().size() == 1);
}

// from
// tests/test_meeting_scheduler.py::TestSegmentWatermark::test_seed_marks_card_and_notes_cursors
TEST_CASE("test_seed_marks_card_and_notes_cursors", "[test_meeting_scheduler]") {
    const std::vector<Json> segments{row("sg_1", 0.0, 5.0, "one"), row("sg_2", 5.0, 10.0, "two")};
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host(segments);
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock, {}, {}, fast_intervals());
    sched.seed_sent_segments(segments);
    const auto sent = P::sent_starts(sched);
    CHECK((sent.size() == 2 && sent.contains("sg_1") && sent.contains("sg_2")));
    const auto notes = P::notes_sent_starts(sched);
    CHECK((notes.size() == 2 && notes.contains("sg_1") && notes.contains("sg_2")));
    CHECK(P::notes_max_sent_start_s(sched) == 5.0);
}

// from
// tests/test_meeting_scheduler.py::TestSegmentWatermark::test_failed_checkpoint_retries_the_same_segments
TEST_CASE("test_failed_checkpoint_retries_the_same_segments", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host({row("sg_1", 0.0, 5.0, "one")});
    FakeAgent agent(1);
    CheckpointScheduler sched(host, agent.core, clock, {}, {}, fast_intervals());
    P::fire(sched);
    P::fire(sched);
    REQUIRE(agent.calls().size() == 2);
    CHECK(ids_of(agent.calls()[1].new_segments) == std::vector<std::string>{"sg_1"});
}

// from
// tests/test_meeting_scheduler.py::TestSegmentWatermark::test_offline_scheduler_recovers_after_a_successful_retry
TEST_CASE("test_offline_scheduler_recovers_after_a_successful_retry", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host({row("sg_1", 0.0, 5.0, "one")});
    FakeAgent agent(3);
    std::vector<bool> health;
    CheckpointScheduler sched(host, agent.core, clock,
                              [&](bool online, const std::string&) { health.push_back(online); });
    for (int i = 0; i < 3; ++i) {
        P::set_pending_segments(sched, 1);
        P::fire(sched);
    }
    CHECK(health == std::vector<bool>{false});
    CHECK_FALSE(sched.online());

    P::set_pending_segments(sched, 1);
    P::fire(sched);
    CHECK(health == std::vector<bool>{false, true});
    CHECK(sched.online());
    CHECK(P::consecutive_failures(sched) == 0);
}

// from
// tests/test_meeting_scheduler.py::TestSegmentWatermark::test_timeout_failures_retain_work_and_three_mark_offline
TEST_CASE("test_timeout_failures_retain_work_and_three_mark_offline", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host({row("sg_1", 0.0, 5.0, "one")});
    FakeAgent agent(3, "Agent pass timed out after 60s");
    std::vector<bool> health;
    CheckpointScheduler sched(host, agent.core, clock,
                              [&](bool online, const std::string&) { health.push_back(online); });
    {
        CapturedLog log;
        for (int i = 0; i < 2; ++i) {
            P::set_pending_segments(sched, 1);
            P::fire(sched);
        }
        CHECK(sched.online());
        CHECK(health.empty());
        P::set_pending_segments(sched, 1);
        P::fire(sched);
        CHECK(health == std::vector<bool>{false});
        CHECK_FALSE(sched.online());
        CHECK(log.contains("Intelligence declared offline"));
        CHECK(log.contains("consecutive_failures="));
        CHECK(log.contains("Firing checkpoint"));
        CHECK(P::pending_segments(sched) >= 1);
    }

    P::set_pending_segments(sched, 1);
    P::fire(sched);
    CHECK(health == std::vector<bool>{false, true});
    CHECK(sched.online());
    CHECK(P::consecutive_failures(sched) == 0);
    CHECK(agent.calls().size() == 4);
    P::fire(sched);
    CHECK(agent.calls().size() == 4);
}

// from
// tests/test_meeting_scheduler.py::TestSegmentWatermark::test_empty_checkpoint_does_not_manufacture_insights
TEST_CASE("test_empty_checkpoint_does_not_manufacture_insights", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host({row("sg_1", 0.0, 5.0, "We should pack up and try the griddle.")});
    host.set_snapshot(Json{{"meeting_id", "m_test"},
                           {"seq", 1},
                           {"cards", Json{{"key_points", Json::array()}}},
                           {"topic", Json{{"current", ""}, {"history", Json::array()}}},
                           {"rolling_summary", ""}});
    const auto store = host.store();
    const auto seq_before = store->seq();
    // QuietAgent: every pass succeeds with no ops.
    auto core = std::make_shared<FakeAgentCore>();
    CheckpointScheduler sched(host, core, clock);
    P::set_pending_segments(sched, 1);
    P::fire(sched);
    // Nothing repairs or seeds the dashboard on a rolling checkpoint (Python patches
    // repair_meeting_state to see it is not called): the store is untouched.
    CHECK(store->seq() == seq_before);
    CHECK(store->snapshot()["cards"]["key_points"] == Json::array());
    const auto passes = core->passes();
    REQUIRE_FALSE(passes.empty());
    CHECK(passes[0].payload.state_snapshot["cards"]["key_points"] == Json::array());
    CHECK(passes[0].payload.new_segments[0]["id"] == "sg_1");
}

// from
// tests/test_meeting_scheduler.py::TestSegmentWatermark::test_prepare_for_end_never_blocks_on_agent_cancel
TEST_CASE("test_prepare_for_end_never_blocks_on_agent_cancel", "[test_meeting_scheduler]") {
    FakeSchedulerHost host;
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, fv::system_clock());
    sched.start();
    sched.prepare_for_end();
    CHECK(P::consolidating(sched));
    CHECK(P::stop_requested(sched));
    CHECK(agent.core->cancel_calls() == 0);
    sched.stop();
}

TEST_CASE("prepare_for_end returns at once while a pass blocks", "[scheduler]") {
    FakeSchedulerHost host({segment("sg_1", 1.0)});
    FakeAgent agent;
    agent.blocking = true;
    agent.released->clear();
    CheckpointScheduler sched(host, agent.core, fv::system_clock());
    sched.start();
    sched.notify_segments(1);
    REQUIRE(agent.entered.wait(2s));
    const auto before = std::chrono::steady_clock::now();
    sched.prepare_for_end();
    const auto took = std::chrono::steady_clock::now() - before;
    CHECK(took < 50ms);
    CHECK(agent.core->cancel_calls() == 0);
    agent.released->set();
    sched.stop();
}

// from
// tests/test_meeting_scheduler.py::TestSegmentWatermark::test_transcript_fetch_failure_restores_claimed_work
TEST_CASE("test_transcript_fetch_failure_restores_claimed_work", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host;
    host.fail_transcript("database busy");
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock, {}, {}, fast_intervals());
    P::set_pending_segments(sched, 4);
    P::fire(sched);
    CHECK(P::pending_segments(sched) == 4);
    CHECK(P::consecutive_failures(sched) == 1);
}

// from
// tests/test_meeting_scheduler.py::TestSegmentWatermark::test_sent_id_set_is_pruned_outside_the_refetch_window
TEST_CASE("test_sent_id_set_is_pruned_outside_the_refetch_window", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host;
    FakeAgent agent;
    auto options = fast_intervals();
    options.refetch_window_s = 20.0;
    CheckpointScheduler sched(host, agent.core, clock, {}, {}, options);
    for (int i = 0; i < 12; ++i) {
        const double start_s = i * 10.0;
        host.add_segments(
            {row("sg_" + std::to_string(i), start_s, start_s + 10.0, "seg " + std::to_string(i))});
        P::fire(sched);
    }
    // Ten-second segments with a 20 s window: only the newest handful can ever be re-fetched, so
    // the set stays small forever.
    const auto sent = P::sent_starts(sched);
    CHECK(sent.size() <= 3);
    CHECK_FALSE(sent.contains("sg_0"));
    CHECK(sent.contains("sg_11"));
    std::size_t cards = 0;
    bool polished = false;
    for (const auto& call : agent.calls()) {
        cards += call.is_polish ? 0 : 1;
        polished = polished || call.is_polish;
    }
    CHECK(cards == 12);
    CHECK(polished);
}

// ---- TestConsolidationRace ----

// from
// tests/test_meeting_scheduler.py::TestConsolidationRace::test_consolidation_skipped_when_worker_will_not_stop
TEST_CASE("test_consolidation_skipped_when_worker_will_not_stop", "[test_meeting_scheduler]") {
    // A worker stuck in a pass that ignores stop() and cancel() entirely.
    FakeSchedulerHost host({row("sg_1", 0.0, 5.0, "one")});
    auto core = std::make_shared<FakeAgentCore>();
    auto wedged = std::make_shared<fv::agents::Event>();
    fv::agents::Event entered;
    core->set_hook([wedged, &entered](const CheckpointPayload&, AgentToolHost*) {
        entered.set();
        wedged->wait(30s);
        AgentResult ok;
        ok.ok = true;
        return ok;
    });
    SchedulerOptions options;
    options.consolidation_join_s = 0.05;
    options.stop_join_s = 0.05;
    CheckpointScheduler sched(host, core, fv::system_clock(), {}, {}, options);
    sched.start();
    sched.notify_segments(1);
    REQUIRE(entered.wait(2s));
    const auto outcome = sched.run_consolidation(1.0);
    // Two agent runs sharing one core is worse than a missing pass.
    for (const auto& pass : core->passes()) {
        CHECK_FALSE(pass.payload.is_consolidation);
    }
    CHECK(outcome.status == "failed");
    CHECK(fv::py::lower(outcome.message).find("still running") != std::string::npos);
    CHECK(host.revoke_calls() == 1);
    wedged->set();
}

// from
// tests/test_meeting_scheduler.py::TestConsolidationRace::test_consolidation_runs_once_the_worker_is_stopped
TEST_CASE("test_consolidation_runs_once_the_worker_is_stopped", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host({row("sg_1", 0.0, 5.0, "one")});
    FakeAgent agent;
    SchedulerOptions options;
    options.consolidation_join_s = 0.05;
    CheckpointScheduler sched(host, agent.core, clock, {}, {}, options);
    const auto outcome = sched.run_consolidation(5.0);
    REQUIRE(agent.calls().size() == 1);
    CHECK(agent.calls()[0].is_consolidation);
    CHECK(outcome == ConsolidationOutcome{"completed", "Final insights are ready."});
    CHECK(host.revoke_calls() == 1);
}

// from tests/test_meeting_scheduler.py::TestConsolidationRace::test_consolidation_unhealthy_agent
TEST_CASE("test_consolidation_unhealthy_agent", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    FakeSchedulerHost host;
    FakeAgent agent;
    agent.core->set_healthy(false);
    CheckpointScheduler sched(host, agent.core, clock);
    const auto outcome = sched.run_consolidation(1.0);
    CHECK(outcome == ConsolidationOutcome{
                         "unavailable", "AI insights are offline; final insights could not run."});
}

// from
// tests/test_meeting_scheduler.py::TestConsolidationRace::test_consolidation_transcript_failure
TEST_CASE("test_consolidation_transcript_failure", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    FakeSchedulerHost host;
    host.fail_transcript("db locked");
    FakeAgent agent;
    const auto outcome = CheckpointScheduler(host, agent.core, clock).run_consolidation();
    CHECK(outcome.status == "failed");
    CHECK(outcome.message == "Could not load the transcript for final insights: db locked");
}

// from tests/test_meeting_scheduler.py::TestConsolidationRace::test_consolidation_missing_store
TEST_CASE("test_consolidation_missing_store", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    FakeSchedulerHost host({row("sg_1", 0.0, 1.0, "x")});
    host.set_store(nullptr);
    FakeAgent agent;
    const auto outcome = CheckpointScheduler(host, agent.core, clock).run_consolidation();
    CHECK(outcome ==
          ConsolidationOutcome{"failed", "Meeting state was unavailable for final insights."});
    CHECK(host.revoke_calls() == 1);
}

// from tests/test_meeting_scheduler.py::TestConsolidationRace::test_consolidation_timeout
TEST_CASE("test_consolidation_timeout", "[test_meeting_scheduler]") {
    // HangingAgent: ignores cancel, so the pass truly produces no result.
    FakeSchedulerHost host({row("sg_1", 0.0, 1.0, "x")});
    auto core = std::make_shared<FakeAgentCore>();
    auto hang = std::make_shared<fv::agents::Event>();
    core->set_hook([hang](const CheckpointPayload&, AgentToolHost*) {
        hang->wait(30s);
        AgentResult ok;
        ok.ok = true;
        return ok;
    });
    SchedulerOptions options;
    options.cancel_grace_s = 0.05;
    CheckpointScheduler sched(host, core, fv::system_clock(), {}, {}, options);
    const auto outcome = sched.run_consolidation(0.05);
    CHECK(outcome.status == "failed");
    CHECK(fv::py::lower(outcome.message).find("timed out") != std::string::npos);
    CHECK(core->cancel_calls() == 1);
    // Revoked before the cancel, and again on the way out.
    CHECK(host.revoke_calls() == 2);
    // The abandoned pass ends once released; it holds the core until then.
    hang->set();
    CHECK(eventually([&] { return core.use_count() == 2; }));
}

TEST_CASE("a consolidation that never returns times out on the scheduler's clock", "[scheduler]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host({row("sg_1", 0.0, 1.0, "x")});
    auto core = std::make_shared<FakeAgentCore>();
    auto hang = std::make_shared<fv::agents::Event>();
    core->set_hook([hang](const CheckpointPayload&, AgentToolHost*) {
        hang->wait(30s);
        AgentResult ok;
        ok.ok = true;
        return ok;
    });
    SchedulerOptions options;
    options.cancel_grace_s = 0.0;
    CheckpointScheduler sched(host, core, clock, {}, {}, options);
    std::vector<std::string> progress;
    auto running = std::async(std::launch::async, [&] {
        return sched.run_consolidation(std::nullopt,
                                       [&](const std::string& line) { progress.push_back(line); });
    });
    REQUIRE(core->wait_for_passes(1, 2s));
    CHECK(running.wait_for(100ms) == std::future_status::timeout);
    clock.advance(899s);
    CHECK(running.wait_for(100ms) == std::future_status::timeout);
    clock.advance(1s);
    REQUIRE(running.wait_for(2s) == std::future_status::ready);
    CHECK(running.get() == ConsolidationOutcome{"failed", "Final insights timed out after 900s."});
    CHECK(progress == std::vector<std::string>{"Synthesizing executive summary, key decisions, and "
                                               "action items over 1 segments..."});
    hang->set();
    CHECK(eventually([&] { return core.use_count() == 2; }));
}

// from tests/test_meeting_scheduler.py::TestConsolidationRace::test_consolidation_agent_failure
TEST_CASE("test_consolidation_agent_failure", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    FakeSchedulerHost host({row("sg_1", 0.0, 1.0, "x")});
    auto core = std::make_shared<FakeAgentCore>();
    core->script(PASS_CONSOLIDATION, failed("model down"));
    const auto outcome = CheckpointScheduler(host, core, clock).run_consolidation();
    CHECK(outcome == ConsolidationOutcome{"failed", "Final insights failed: model down"});
}

TEST_CASE("consolidation repairs the state even when the agent fails", "[scheduler]") {
    // repair_meeting_state fills a blank topic and summary from the synthesized key points, so
    // the durable record has them whatever the agent did.
    fv::FakeClock clock;
    FakeSchedulerHost host({row("sg_1", 0.0, 1.0, "We decided to ship on Friday.")});
    host.set_snapshot(Json{
        {"meeting_id", "m_test"},
        {"cards", Json{{"key_points", Json::array({Json{{"id", "it_1"},
                                                        {"card", "key_points"},
                                                        {"text", "We ship on Friday."},
                                                        {"status", "proposed"},
                                                        {"evidence", Json::array({"sg_1"})}}})}}}});
    auto core = std::make_shared<FakeAgentCore>();
    core->script(PASS_CONSOLIDATION, failed("model down"));
    const auto before = host.store()->seq();
    const auto outcome = CheckpointScheduler(host, core, clock).run_consolidation();
    CHECK(outcome == ConsolidationOutcome{"failed", "Final insights failed: model down"});
    CHECK(host.store()->seq() > before);
    CHECK(host.store()->snapshot()["rolling_summary"] != "");
}

// ---- TestFinalPolish ----

// from
// tests/test_meeting_scheduler.py::TestFinalPolish::test_rolling_polish_is_bounded_and_keeps_the_newest_speech
TEST_CASE("test_rolling_polish_is_bounded_and_keeps_the_newest_speech",
          "[test_meeting_scheduler]") {
    const std::string text = GENERATE(std::string("short line"), std::string(1000, 'x'));
    const auto segments = numbered(435, text);
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host(segments);
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock);
    P::set_successful_checkpoints(sched, 6);
    P::maybe_fire_polish(sched);
    REQUIRE(agent.calls().size() == 1);
    const auto block = agent.calls()[0].new_segments;
    CHECK(!block.empty());
    CHECK(block.size() <= 80);
    std::size_t chars = 0;
    for (const auto& r : block) {
        chars += fv::py::len(r["text"].get<std::string>());
    }
    CHECK(chars <= 8000);
    CHECK(block == std::vector<Json>(segments.end() - static_cast<std::ptrdiff_t>(block.size()),
                                     segments.end()));
}

// from
// tests/test_meeting_scheduler.py::TestFinalPolish::test_final_polish_uses_full_transcript_and_is_polish
TEST_CASE("test_final_polish_uses_full_transcript_and_is_polish", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    FakeSchedulerHost host({row("sg_1", 0.0, 5.0, "hello"), row("sg_2", 5.0, 10.0, "world")});
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock);
    PolishStandIn polish;
    const auto outcome = sched.run_final_polish(polish.fn(), 5.0);
    CHECK(outcome == ConsolidationOutcome{"completed", "Transcript cleanup is ready."});
    REQUIRE(polish.seen.size() == 1);
    CHECK(ids_of(polish.seen[0]) == std::vector<std::string>{"sg_1", "sg_2"});
    REQUIRE(agent.calls().size() == 1);
    CHECK(agent.calls()[0].is_polish);
    CHECK_FALSE(agent.calls()[0].is_consolidation);
    CHECK(ids_of(agent.calls()[0].new_segments) == std::vector<std::string>{"sg_1", "sg_2"});
    // The final polish leaves the write gate to the consolidation that follows.
    CHECK(host.revoke_calls() == 0);
}

// from
// tests/test_meeting_scheduler.py::TestFinalPolish::test_final_polish_then_consolidation_see_same_segments
TEST_CASE("test_final_polish_then_consolidation_see_same_segments", "[test_meeting_scheduler]") {
    fv::FakeClock clock;
    FakeSchedulerHost host({row("sg_final", 1.0, 2.0, "clean")});
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock);
    PolishStandIn polish;
    const auto polished = sched.run_final_polish(polish.fn(), 5.0);
    const auto report = sched.run_consolidation(5.0);
    CHECK(polished.status == "completed");
    CHECK(report.status == "completed");
    const auto calls = agent.calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].is_polish);
    CHECK(calls[1].is_consolidation);
    CHECK(calls[0].new_segments[0]["id"] == "sg_final");
    CHECK(calls[1].new_segments[0]["id"] == "sg_final");
}

TEST_CASE("the final polish says why it could not run", "[scheduler]") {
    fv::FakeClock clock;
    PolishStandIn polish;
    SECTION("an unhealthy agent") {
        FakeSchedulerHost host({row("sg_1", 0.0, 1.0, "x")});
        FakeAgent agent;
        agent.core->set_healthy(false);
        CHECK(CheckpointScheduler(host, agent.core, clock).run_final_polish(polish.fn()) ==
              ConsolidationOutcome{
                  "unavailable",
                  "Meeting intelligence is offline; transcript cleanup could not run."});
    }
    SECTION("no transcript") {
        FakeSchedulerHost host;
        FakeAgent agent;
        CHECK(CheckpointScheduler(host, agent.core, clock).run_final_polish(polish.fn()) ==
              ConsolidationOutcome{"completed", "No transcript text needed cleanup."});
    }
    SECTION("a failed transcript read") {
        FakeSchedulerHost host;
        host.fail_transcript("db locked");
        FakeAgent agent;
        CHECK(
            CheckpointScheduler(host, agent.core, clock).run_final_polish(polish.fn()) ==
            ConsolidationOutcome{"failed", "Could not load the transcript for cleanup: db locked"});
    }
    SECTION("no store") {
        FakeSchedulerHost host({row("sg_1", 0.0, 1.0, "x")});
        host.set_store(nullptr);
        FakeAgent agent;
        CHECK(CheckpointScheduler(host, agent.core, clock).run_final_polish(polish.fn()) ==
              ConsolidationOutcome{"failed",
                                   "Meeting state was unavailable for transcript cleanup."});
    }
    SECTION("a block that failed") {
        FakeSchedulerHost host({row("sg_1", 0.0, 1.0, "x")});
        FakeAgent agent(1, "second block failed");
        CHECK(CheckpointScheduler(host, agent.core, clock).run_final_polish(polish.fn()) ==
              ConsolidationOutcome{"failed", "second block failed"});
    }
    SECTION("an empty reason is no failure") {
        // Python's `if error:` reads "" as success.
        FakeSchedulerHost host({row("sg_1", 0.0, 1.0, "x")});
        FakeAgent agent;
        const PolishTranscript empty_reason =
            [](AgentCore&, fv::meeting_state::MeetingStateStore&, const std::vector<Json>&, double,
               const std::function<void(const std::string&, int, int)>&) {
                return std::optional<std::string>("");
            };
        CHECK(CheckpointScheduler(host, agent.core, clock).run_final_polish(empty_reason) ==
              ConsolidationOutcome{"completed", "Transcript cleanup is ready."});
    }
    CHECK(polish.seen.size() <= 1);
}

// ---- TestTopicShift ----

// from tests/test_meeting_scheduler.py::TestTopicShift::test_content_words_filters_stopwords
TEST_CASE("test_content_words_filters_stopwords", "[test_meeting_scheduler]") {
    const auto words =
        content_words("Yeah okay we should discuss the quarterly budget review process");
    CHECK_FALSE(words.contains("yeah"));
    CHECK_FALSE(words.contains("okay"));
    CHECK(words.contains("budget"));
    CHECK(words.contains("quarterly"));
}

namespace {

const std::vector<Json> kOlder{Json{{"start_s", 85.0},
                                    {"end_s", 95.0},
                                    {"text", "budget forecast revenue pipeline margin earnings"}},
                               Json{{"start_s", 100.0},
                                    {"end_s", 115.0},
                                    {"text", "quarterly finance spreadsheet ledger accounting"}}};
const std::vector<Json> kNewer{Json{{"start_s", 145.0},
                                    {"end_s", 155.0},
                                    {"text", "hiring onboarding interview candidates staffing"}},
                               Json{{"start_s", 165.0},
                                    {"end_s", 185.0},
                                    {"text", "recruitment culture retention engineers designers"}}};

std::vector<Json> joined(std::vector<Json> a, const std::vector<Json>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

SchedulerOptions shift_options() {
    SchedulerOptions options;
    options.base_interval_s = 45.0;
    options.min_interval_s = 0.05;
    options.max_interval_s = 60.0;
    options.shift_check_spacing_s = 0.0;
    return options;
}

} // namespace

// from tests/test_meeting_scheduler.py::TestTopicShift::test_jaccard_topic_shift_early_fire
TEST_CASE("test_jaccard_topic_shift_early_fire", "[test_meeting_scheduler]") {
    // Two 60 s windows with disjoint content words (jaccard ~ 0). The cursor is now_s - 120 and
    // the read is start_s > cursor, so the older starts stay above 80 when now_s is 200.
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host(joined(kOlder, kNewer), {.clock_s = 200.0});
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock, {}, {}, shift_options());
    // Elapsed past min but not past base (45 s): only a topic shift can fire.
    P::set_last_fire_mono(sched, P::mono(sched) - 5.0);
    P::set_last_shift_check_mono(sched, 0.0);
    P::set_pending_segments(sched, 4);
    CHECK(P::detect_topic_shift(sched));

    // Similar windows: no shift.
    host.set_segments(
        joined(kOlder, {Json{{"start_s", 145.0},
                             {"end_s", 155.0},
                             {"text", "budget forecast revenue pipeline margin earnings"}},
                        Json{{"start_s", 165.0},
                             {"end_s", 185.0},
                             {"text", "quarterly finance spreadsheet ledger accounting"}}}));
    P::set_last_shift_check_mono(sched, 0.0);
    CHECK_FALSE(P::detect_topic_shift(sched));
}

// from
// tests/test_meeting_scheduler.py::TestTopicShift::test_topic_shift_triggers_fire_when_interval_not_due
TEST_CASE("test_topic_shift_triggers_fire_when_interval_not_due", "[test_meeting_scheduler]") {
    FakeSchedulerHost host(joined(kOlder, kNewer), {.clock_s = 200.0});
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, fv::system_clock(), {}, {}, shift_options());
    P::set_last_fire_mono(sched, P::mono(sched) - 5.0); // < base 45 s
    P::set_last_shift_check_mono(sched, 0.0);
    sched.start();
    sched.notify_segments(4);
    CHECK(eventually([&] { return !agent.calls().empty(); }));
    sched.stop();
}

// ---- tests/test_meeting_notes_agent.py TestNotesScheduler ----

// from tests/test_meeting_notes_agent.py::TestNotesScheduler::test_notes_pass_fires_for_any_core
TEST_CASE("test_notes_pass_fires_for_any_core", "[test_meeting_notes_agent]") {
    fv::FakeClock clock;
    start_late(clock);
    auto host = notes_host({segment("sg_1", 10.0)});
    FakeNotesAgent agent;
    CheckpointScheduler sched(*host, agent.core, clock);
    P::set_successful_checkpoints(sched, 6);
    P::maybe_fire_notes(sched);
    CHECK(agent.calls().size() == 1);
}

// from
// tests/test_meeting_notes_agent.py::TestNotesScheduler::test_notes_payload_carries_flag_and_consumes_segments
TEST_CASE("test_notes_payload_carries_flag_and_consumes_segments", "[test_meeting_notes_agent]") {
    fv::FakeClock clock;
    start_late(clock);
    auto host = notes_host({segment("sg_1", 10.0), segment("sg_2", 20.0)});
    FakeNotesAgent agent;
    CheckpointScheduler sched(*host, agent.core, clock);
    P::set_successful_checkpoints(sched, 2);
    P::maybe_fire_notes(sched);

    REQUIRE(agent.calls().size() == 1);
    const auto payload = agent.calls()[0];
    CHECK(payload.is_notes);
    CHECK_FALSE(payload.is_polish);
    CHECK_FALSE(payload.is_consolidation);
    CHECK(ids_of(payload.new_segments) == std::vector<std::string>{"sg_1", "sg_2"});
    CHECK(P::notes_max_sent_start_s(sched) == 20.0);

    // Consumed segments do not refire even when the cadence is due.
    P::set_successful_checkpoints(sched, 4);
    P::maybe_fire_notes(sched);
    CHECK(agent.calls().size() == 1);
}

// from
// tests/test_meeting_notes_agent.py::TestNotesScheduler::test_notes_seed_after_first_checkpoint
TEST_CASE("test_notes_seed_after_first_checkpoint", "[test_meeting_notes_agent]") {
    fv::FakeClock clock;
    start_late(clock);
    auto host = notes_host({segment("sg_1", 10.0)});
    FakeNotesAgent agent;
    CheckpointScheduler sched(*host, agent.core, clock);
    P::set_successful_checkpoints(sched, 1);
    P::maybe_fire_notes(sched);
    REQUIRE(agent.calls().size() == 1);
    CHECK(agent.calls()[0].is_notes);
}

// from
// tests/test_meeting_notes_agent.py::TestNotesScheduler::test_failed_notes_pass_leaves_segments_for_retry
TEST_CASE("test_failed_notes_pass_leaves_segments_for_retry", "[test_meeting_notes_agent]") {
    fv::FakeClock clock;
    start_late(clock);
    auto host = notes_host({segment("sg_1", 10.0)});
    FakeNotesAgent agent(1);
    CheckpointScheduler sched(*host, agent.core, clock);
    P::set_successful_checkpoints(sched, 2);

    P::maybe_fire_notes(sched); // fails
    CHECK(agent.calls().size() == 1);
    CHECK(P::notes_max_sent_start_s(sched) < 0.0); // nothing marked consumed
    // The cadence moved: no immediate hot-loop retry.
    P::maybe_fire_notes(sched);
    CHECK(agent.calls().size() == 1);

    // After another successful checkpoint the window is retried and wins.
    P::set_successful_checkpoints(sched, 4);
    P::set_notes_retry_not_before(sched, 0.0);
    P::maybe_fire_notes(sched);
    CHECK(agent.calls().size() == 2);
    CHECK(P::notes_max_sent_start_s(sched) == 10.0);
}

// from
// tests/test_meeting_notes_agent.py::TestNotesScheduler::test_notes_pass_skipped_without_new_segments
TEST_CASE("test_notes_pass_skipped_without_new_segments", "[test_meeting_notes_agent]") {
    fv::FakeClock clock;
    start_late(clock);
    auto host = notes_host({});
    FakeNotesAgent agent;
    CheckpointScheduler sched(*host, agent.core, clock);
    P::set_successful_checkpoints(sched, 5);
    P::maybe_fire_notes(sched);
    CHECK(agent.calls().empty());
}

// ---- tests/test_meeting_corrections.py TestSchedulerGuidance ----

// from
// tests/test_meeting_corrections.py::TestSchedulerGuidance::test_guidance_fires_without_new_speech_and_resends_recent_transcript
TEST_CASE("test_guidance_fires_without_new_speech_and_resends_recent_transcript",
          "[test_meeting_corrections]") {
    std::vector<Json> segments;
    for (std::size_t i = 0; i < GUIDANCE_MAX_SEGMENTS + 5; ++i) {
        segments.push_back(segment("sg_" + std::to_string(i), static_cast<double>(i)));
    }
    auto host = notes_host(segments);
    FakeNotesAgent agent;
    CheckpointScheduler sched(*host, agent.core, fv::system_clock(), {}, {}, fast_intervals(60.0));
    // Everything is already known to the agent; a normal tick would not fire.
    P::mark_sent(sched, segments);
    sched.start();
    sched.notify_guidance();
    CHECK(eventually([&] { return !agent.calls().empty(); }));
    std::vector<CheckpointPayload> cards;
    for (const auto& call : agent.calls()) {
        if (!call.is_notes) {
            cards.push_back(call);
        }
    }
    REQUIRE(cards.size() == 1);
    CHECK_FALSE(cards[0].is_consolidation);
    const auto sent = ids_of(cards[0].new_segments);
    CHECK(sent.size() == GUIDANCE_MAX_SEGMENTS);
    CHECK(sent.back() == "sg_" + std::to_string(GUIDANCE_MAX_SEGMENTS + 4));
    CHECK_FALSE(P::guidance_pending(sched));
    sched.stop();
}

// from
// tests/test_meeting_corrections.py::TestSchedulerGuidance::test_failed_guidance_pass_is_retried
TEST_CASE("test_failed_guidance_pass_is_retried", "[test_meeting_corrections]") {
    fv::FakeClock clock;
    start_late(clock);
    auto host = notes_host({segment("sg_1", 1.0)});
    FakeNotesAgent agent(1);
    CheckpointScheduler sched(*host, agent.core, clock);
    P::mark_sent(sched, host->segments());
    sched.notify_guidance();
    P::fire(sched);
    CHECK(agent.calls().size() == 1);
    CHECK(P::guidance_pending(sched));
    P::set_retry_not_before(sched, 0.0);
    P::fire(sched);
    std::size_t cards = 0;
    for (const auto& call : agent.calls()) {
        cards += call.is_notes ? 0 : 1;
    }
    CHECK(cards == 2);
    CHECK_FALSE(P::guidance_pending(sched));
}

// ---- the pieces the traces do not show ----

TEST_CASE("backoff grows 30, 60, 120 and 300 s and guidance clears it once", "[scheduler]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host({segment("sg_1", 1.0)});
    FakeAgent agent(5);
    CheckpointScheduler sched(host, agent.core, clock);
    std::vector<double> delays;
    for (int i = 0; i < 5; ++i) {
        P::set_pending_segments(sched, 1);
        P::fire(sched);
        delays.push_back(P::retry_not_before(sched) - P::mono(sched));
    }
    CHECK(delays == std::vector<double>{30.0, 60.0, 120.0, 300.0, 300.0});
    sched.notify_guidance();
    CHECK(P::retry_not_before(sched) == 0.0);
    CHECK(P::notes_retry_not_before(sched) == 0.0);
}

TEST_CASE("a failed cursor save counts as a checkpoint failure", "[scheduler]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host({segment("sg_1", 1.0)}, {.ledger = true});
    host.fail_advance("disk full");
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock);
    P::set_pending_segments(sched, 1);
    P::fire(sched);
    CHECK(card_passes(agent.calls()).size() == 1);
    CHECK(P::consecutive_failures(sched) == 1);
    CHECK(P::pending_segments(sched) == 1);
    CHECK(P::successful_checkpoints(sched) == 0);
}

TEST_CASE("an on_health callback that throws is swallowed", "[scheduler]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host({segment("sg_1", 1.0)});
    FakeAgent agent(3);
    CheckpointScheduler sched(host, agent.core, clock,
                              [](bool, const std::string&) { throw std::runtime_error("boom"); });
    for (int i = 0; i < 3; ++i) {
        P::set_pending_segments(sched, 1);
        CHECK_NOTHROW(P::fire(sched));
    }
    CHECK_FALSE(sched.online());
}

TEST_CASE("a missing store requeues the claimed work and the guidance", "[scheduler]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host({segment("sg_1", 1.0)});
    host.set_store(nullptr);
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock);
    sched.notify_guidance();
    P::set_pending_segments(sched, 3);
    P::fire(sched);
    CHECK(agent.calls().empty());
    // notify_guidance adds one more pending notification on top of the three claimed.
    CHECK(P::pending_segments(sched) == 4);
    CHECK(P::guidance_pending(sched));
    CHECK(P::consecutive_failures(sched) == 0);
}

TEST_CASE("start reads the backlog and a scheduler starts only once", "[scheduler]") {
    FakeSchedulerHost host({segment("sg_1", 1.0)}, {.ledger = true});
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, fv::system_clock());
    sched.start();
    sched.start();
    CHECK(P::worker_running(sched));
    // The backlog (one row above the cards cursor) wakes the worker at once.
    CHECK(eventually([&] { return !card_passes(agent.calls()).empty(); }));
    sched.stop();
    CHECK_FALSE(P::worker_running(sched));
    CHECK(host.cursor_acks().front() == std::pair<std::string, std::int64_t>{"cards", 1});
}

TEST_CASE("a pass that throws a non-standard exception fails that pass only", "[scheduler]") {
    // Python's `except Exception` covers whatever a core raises: the pass fails, the requester
    // hears back and the worker keeps serving.
    FakeSchedulerHost host({segment("sg_1", 1.0)});
    auto core = std::make_shared<FakeAgentCore>();
    core->set_hook([](const CheckpointPayload&, AgentToolHost*) -> AgentResult { throw 42; });
    SECTION("a card checkpoint") {
        fv::FakeClock clock;
        start_late(clock);
        CheckpointScheduler sched(host, core, clock);
        P::set_pending_segments(sched, 2);
        CHECK_NOTHROW(P::fire(sched));
        CHECK(P::consecutive_failures(sched) == 1);
        CHECK(P::pending_segments(sched) == 2);
    }
    SECTION("a note request on the worker") {
        CheckpointScheduler sched(host, core, fv::system_clock());
        sched.start();
        auto request = sched.request_note_adjustment("Shorten");
        REQUIRE(request.wait_for(3s) == std::future_status::ready);
        const auto result = request.get();
        CHECK_FALSE(result.ok);
        CHECK(result.error == "unknown error");
        CHECK(P::worker_running(sched));
        auto again = sched.request_note_adjustment("Shorten again");
        REQUIRE(again.wait_for(3s) == std::future_status::ready);
        CHECK(again.get().error == "unknown error");
        sched.stop();
    }
}
