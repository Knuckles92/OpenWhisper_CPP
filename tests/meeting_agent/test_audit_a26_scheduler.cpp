// Audit AU18 (docs/port/audit/a26-meeting-agent.json, A26-47): the two end-of-meeting passes
// refuse to run beside a rolling pass that will not stop, with scheduler.py's sentences
// (scheduler.py:1186-1195 and 1287-1297), and still close the agent-write gate for the
// consolidation only.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "meeting_agent/scheduler.h"
#include "meeting_agent/testing/fake_agent_core.h"
#include "meeting_agent/testing/fake_scheduler_host.h"
#include "scheduler_support.h"

namespace {

using namespace std::chrono_literals;
using namespace fv::meeting_agent;
using fv::Json;
using namespace fv::meeting_agent::scheduler_test;

TEST_CASE("the final passes say so when a rolling pass will not stop", "[audit][scheduler]") {
    const bool consolidation = GENERATE(true, false);
    CAPTURE(consolidation);
    FakeSchedulerHost host({row("sg_1", 0.0, 5.0, "one")});
    auto core = std::make_shared<FakeAgentCore>();
    auto wedged = std::make_shared<fv::agents::Event>();
    auto entered = std::make_shared<fv::agents::Event>();
    core->set_hook([wedged, entered](const CheckpointPayload&, AgentToolHost*) {
        entered->set();
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
    REQUIRE(entered->wait(2s));

    bool polished = false;
    const PolishTranscript polish = [&](AgentCore&, fv::meeting_state::MeetingStateStore&,
                                        const std::vector<Json>&, double,
                                        const std::function<void(const std::string&, int, int)>&) {
        polished = true;
        return std::optional<std::string>();
    };
    const auto outcome =
        consolidation ? sched.run_consolidation(1.0) : sched.run_final_polish(polish);
    wedged->set();
    if (consolidation) {
        CHECK(outcome == ConsolidationOutcome{"failed", "Final insights were skipped because a "
                                                        "previous checkpoint was still running."});
        CHECK(host.revoke_calls() == 1);
    } else {
        CHECK(outcome == ConsolidationOutcome{"failed",
                                              "Transcript cleanup was skipped because a previous "
                                              "checkpoint was still running."});
        // The final polish leaves the gate open for the consolidation that follows it.
        CHECK(host.revoke_calls() == 0);
        CHECK_FALSE(polished);
    }
    for (const auto& pass : core->passes()) {
        CHECK_FALSE(pass.payload.is_consolidation);
    }
}

} // namespace
