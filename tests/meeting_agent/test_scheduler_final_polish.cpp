// The scheduler's half of the final transcript cleanup (test_meeting_scheduler.py's
// TestFinalPolish): run_final_polish hands the whole final transcript, the per-block budget and
// the progress sink to the engine's polish_transcript, and reports its failure as it was
// worded. The blocks themselves and their wording are polish_transcript's (src/meeting).
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <tuple>
#include <vector>

#include "base/testing/fake_clock.h"
#include "meeting_agent/scheduler.h"
#include "meeting_agent/testing/fake_agent_core.h"
#include "meeting_agent/testing/fake_scheduler_host.h"
#include "scheduler_support.h"

namespace {

using fv::Json;
using namespace fv::meeting_agent;
using namespace fv::meeting_agent::scheduler_test;

TEST_CASE("the final polish passes its budget and progress through and keeps the failure's words",
          "[scheduler]") {
    fv::FakeClock clock;
    std::vector<Json> rows;
    for (int i = 0; i < 3; ++i) {
        rows.push_back(row("sg_" + std::to_string(i), i, i + 1, "draft"));
    }
    FakeSchedulerHost host(rows);
    FakeAgent agent;
    CheckpointScheduler scheduler(host, agent.core, clock);
    std::vector<std::tuple<std::string, int, int>> heard;
    double budget = 0.0;
    std::vector<Json> given;
    const PolishTranscript polish =
        [&](AgentCore&, fv::meeting_state::MeetingStateStore&, const std::vector<Json>& segments,
            double timeout_s, const std::function<void(const std::string&, int, int)>& progress)
        -> std::optional<std::string> {
        budget = timeout_s;
        given = segments;
        progress("Cleaning transcript (block 1/2)…", 1, 2);
        progress("Cleaning transcript (block 2/2)…", 2, 2);
        return "Transcript cleanup stopped at block 2/2 (request polish-2): timed out";
    };
    const auto outcome = scheduler.run_final_polish(
        polish, 7.5, [&](const std::string& detail, int current, int total) {
            heard.emplace_back(detail, current, total);
        });
    CHECK(budget == 7.5);
    CHECK(given == rows);
    CHECK(heard == std::vector<std::tuple<std::string, int, int>>{
                       {"Cleaning transcript (block 1/2)…", 1, 2},
                       {"Cleaning transcript (block 2/2)…", 2, 2}});
    CHECK(outcome.status == "failed");
    CHECK(outcome.message ==
          "Transcript cleanup stopped at block 2/2 (request polish-2): timed out");
    // A failed cleanup leaves the agent's write gate to the consolidation.
    CHECK(host.revoke_calls() == 0);
}

} // namespace
