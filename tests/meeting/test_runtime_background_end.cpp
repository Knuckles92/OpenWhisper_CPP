// tests/test_meeting_engine.py::test_background_end_persists_report_while_next_meeting_runs: the
// runtime over real engines. A meeting whose final report is still being written moves to the
// background, the next meeting starts, and the report lands on the first meeting's record
// without touching the second.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "engine_support.h"
#include "pyport/json.h"
#include "runtime_support.h"

using fv::Json;
using fv::meeting::MeetingEngine;
using fv::meeting::test::EngineRig;
using fv::meeting::test::EngineSetup;
using fv::meeting::test::Event;
using namespace fv::meeting::runtime_test;

namespace {

using std::chrono::seconds;

// A MeetingEngine of the engine rig as the runtime drives one (the app's adapter also owns the
// engine and its dashboard routes).
class RealEngine final : public fv::meeting::RuntimeEngine {
public:
    explicit RealEngine(MeetingEngine& engine) : engine_(engine) {}

    std::string meeting_id() const override { return engine_.meeting_id(); }
    bool is_active() const override { return engine_.is_active(); }
    fv::Subscription
    add_listener(fv::Executor& executor,
                 std::function<void(const fv::meeting::MeetingEvent&)> fn) override {
        return engine_.add_listener(executor, std::move(fn));
    }
    Json start() override { return engine_.start(); }
    void abort_start() override { engine_.shutdown(); }
    bool processing_stopped() const override { return !engine_.is_active(); }
    void pause() override { engine_.pause(); }
    void resume() override { engine_.resume(); }
    void end() override { engine_.end(); }
    bool wait_for_end(std::chrono::milliseconds timeout) override {
        return engine_.wait_for_end(timeout);
    }
    void shutdown() override { engine_.shutdown(); }
    bool publish_finalization(const Json& finalization) override {
        return engine_.publish_finalization(finalization);
    }
    std::shared_ptr<fv::meeting_state::MeetingStateStore> store() const override {
        return engine_.store();
    }
    double meeting_time_s() const override { return engine_.clock().now_s(); }
    void allow_agent_writes() override { engine_.allow_agent_writes(); }
    void revoke_agent_writes() override { engine_.revoke_agent_writes(); }
    std::shared_ptr<fv::meeting::DashboardTitles> dashboard() const override { return nullptr; }

private:
    MeetingEngine& engine_;
};

// Final passes whose report waits for the test.
class SlowReport final : public fv::meeting::test::OnlineProvider {
public:
    Event entered;
    Event release;

    Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
        return {{"status", "completed"}};
    }
    Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
        entered.set();
        if (!release.wait(seconds(10))) {
            return {{"status", "failed"}, {"message", "The fixture release timed out."}};
        }
        return {{"status", "completed"}, {"message", "Report ready"}};
    }
};

} // namespace

TEST_CASE("test_background_end_persists_report_while_next_meeting_runs", "[test_meeting_engine]") {
    // from
    // tests/test_meeting_engine.py::test_background_end_persists_report_while_next_meeting_runs
    EngineRig engines;
    auto report = std::make_shared<SlowReport>();
    RuntimeRig rig(
        [&engines](fv::meeting::MeetingRuntimeDeps& deps) { deps.repository = engines.sql.repo; });
    // The background meeting is waited for on a thread of its own, as in the app.
    std::vector<std::thread> waiters;
    rig.divert = [&waiters](const std::string& name, fv::Task& task) {
        if (!name.starts_with("meeting-background-m_")) {
            return false;
        }
        waiters.emplace_back(std::move(task));
        return true;
    };
    auto probe = rig.probe();

    EngineSetup old_setup{.options = EngineRig::default_options()};
    old_setup.intelligence = report;
    MeetingEngine& old_engine = engines.make(std::move(old_setup));
    probe.set_engine(std::make_shared<RealEngine>(old_engine), true);
    old_engine.start();
    const std::string old_id = old_engine.meeting_id();
    rig.drain();

    old_engine.end();
    REQUIRE(report->entered.wait());
    // ended, the model's release and "background ready" reached the desktop; the report runs.
    rig.drain();
    REQUIRE(rig.rt->is_finalizing());
    rig.rt->continue_in_background();
    rig.drain();
    REQUIRE(probe.background_ids() == std::vector<std::string>{old_id});
    REQUIRE(waiters.size() == 1);
    CHECK_FALSE(probe.engine());

    EngineSetup new_setup{.options = EngineRig::default_options()};
    new_setup.options.cloud_enabled = false;
    MeetingEngine& new_engine = engines.make(std::move(new_setup));
    probe.set_engine(std::make_shared<RealEngine>(new_engine), true);
    new_engine.start();
    // Speaker readiness reports from its own thread; let it settle before watching only the
    // old meeting's completion.
    REQUIRE(new_engine.wait_for_processing(seconds(5)));
    rig.drain();
    probe.set_controller_active(true);
    rig.got = Recorded{};
    const auto lease_before = rig.lease->calls();

    report->release.set();
    for (std::thread& waiter : waiters) {
        waiter.join();
    }
    rig.drain();

    CHECK(new_engine.is_active());
    CHECK(probe.controller_active());
    CHECK(probe.engine() != nullptr);
    // The dictation model is not handed back under the running meeting.
    CHECK(rig.lease->calls() == lease_before);
    REQUIRE_FALSE(rig.got.states.empty());
    for (const Json& state : rig.got.states) {
        CHECK(state.size() == 1);
        CHECK(state.contains("background_message"));
    }
    CHECK(rig.got.errors.empty());
    const Json stored = engines.sql.repo->get_meeting(old_id).value();
    const Json finalization =
        fv::py::loads(stored.at("state_json").get<std::string>()).at("finalization");
    CHECK(finalization.at("status") == "completed");
    CHECK(finalization.at("card_deferred") == true);
    // The old meeting's dashboard is closed; the new one's keeps serving.
    CHECK(engines.server(0)->stopped.load());
    CHECK_FALSE(engines.server(1)->stopped.load());
    CHECK(probe.background_ids().empty());
    CHECK(engines.sql.repo->get_meeting(new_engine.meeting_id())->at("status") == "active");
}
