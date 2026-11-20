// tests/test_meeting_runtime.py: the finalization card (restore at startup, Keep for later, past
// meetings shown on it) and background finishing.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "meeting/runtime_messages.h"
#include "pyport/json.h"
#include "runtime_support.h"

using namespace fv::meeting;
using namespace fv::meeting::runtime_test;
using fv::Json;

namespace {

bool any_contains(const std::vector<std::string>& lines, std::string_view text) {
    return std::ranges::any_of(
        lines, [text](const std::string& line) { return line.find(text) != std::string::npos; });
}

// Python's background_meeting fixture: an ended meeting still finalizing, its local model free,
// whose End is waited for on a real thread until the test releases it.
struct BackgroundMeeting {
    explicit BackgroundMeeting(bool with_store = true) {
        engine = std::make_shared<FakeEngine>("m_background");
        engine->active = false;
        if (with_store) {
            engine->set_finalization(Json{{"status", "completed"}, {"steps", Json::array()}});
        }
        engine->on_wait = [this](std::chrono::milliseconds timeout) {
            std::unique_lock lock(mutex);
            return released_cv.wait_for(lock, timeout, [this] { return released; });
        };
        rig.divert = [this](const std::string& name, fv::Task& task) {
            if (name != "meeting-background-m_background") {
                return false;
            }
            threads.emplace_back(std::move(task));
            return true;
        };
        auto probe = rig.probe();
        probe.set_engine(engine, true);
        probe.set_card(engine->meeting_id());
        probe.set_finalizing(true);
        probe.set_finalization(Json{{"status", "running"}});
        probe.set_background_ready(true);
    }
    ~BackgroundMeeting() { release(); }

    // continue_in_background, its persistence worker, then its continuation on the UI actor.
    void handoff() {
        rig.rt->continue_in_background();
        rig.workers.run_all();
        rig.ui.run_all();
    }
    void release() {
        {
            const std::lock_guard lock(mutex);
            released = true;
        }
        released_cv.notify_all();
        for (std::thread& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    RuntimeRig rig;
    std::shared_ptr<FakeEngine> engine;
    std::mutex mutex;
    std::condition_variable released_cv;
    bool released = false;
    std::vector<std::thread> threads;
};

} // namespace

// from
// tests/test_meeting_runtime.py::test_restore_deferred_latest_leaves_idle_and_does_not_hydrate_older
TEST_CASE("test_restore_deferred_latest_leaves_idle_and_does_not_hydrate_older",
          "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_ended_meeting("m_new", true);
    rig.make_ended_meeting("m_old", false, "failed", "2026-08-18T18:00:00");
    auto probe = rig.probe();
    probe.restore_last_finalization_worker();
    rig.drain();
    CHECK_FALSE(probe.card());
    CHECK_FALSE(probe.finalization());
    CHECK(rig.got.states.empty());
}

// from tests/test_meeting_runtime.py::test_restore_all_deferred_leaves_idle
TEST_CASE("test_restore_all_deferred_leaves_idle", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_ended_meeting("m_a", true);
    rig.make_ended_meeting("m_b", true);
    auto probe = rig.probe();
    probe.restore_last_finalization_worker();
    rig.drain();
    CHECK_FALSE(probe.card());
    CHECK_FALSE(probe.finalization());
    CHECK(rig.got.states.empty());
}

// from tests/test_meeting_runtime.py::test_hydrate_skips_pending_without_steps
TEST_CASE("test_hydrate_skips_pending_without_steps", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_meeting("m_pending",
                     Json{{"cloud_enabled", true},
                          {"state_json", R"({"meeting_id":"m_pending","status":"canceled",)"
                                         R"("finalization":{"status":"pending"}})"}},
                     "canceled");
    auto probe = rig.probe();
    probe.restore_last_finalization_worker();
    rig.drain();
    CHECK_FALSE(probe.card());
}

// from tests/test_meeting_runtime.py::test_defer_persists_flag_and_hides_card
TEST_CASE("test_defer_persists_flag_and_hides_card", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_ended_meeting("m_card");
    auto probe = rig.probe();
    probe.set_card("m_card");
    probe.set_finalization(Json{{"status", "failed"}, {"message", "boom"}});

    rig.rt->defer_finalization_card();
    rig.drain();

    CHECK_FALSE(probe.finalization());
    CHECK_FALSE(probe.card());
    const Json saved = rig.saved_finalization("m_card");
    CHECK(saved["card_deferred"] == true);
    CHECK(saved["status"] == "failed");
    REQUIRE_FALSE(rig.got.states.empty());
    CHECK(rig.got.states.back()["finalization"].is_null());
    CHECK(rig.got.statuses.back() == messages::kSavedForLater);
}

// from tests/test_meeting_runtime.py::test_defer_completed_card_uses_saved_status
TEST_CASE("test_defer_completed_card_uses_saved_status", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_ended_meeting("m_done", false, "completed");
    auto probe = rig.probe();
    probe.set_card("m_done");
    probe.set_finalization(Json{{"status", "completed"}, {"message", "ready"}});

    rig.rt->defer_finalization_card();
    rig.drain();

    CHECK_FALSE(probe.finalization());
    CHECK(rig.saved_finalization("m_done")["card_deferred"] == true);
    CHECK(std::ranges::count(rig.got.statuses,
                             std::string("Meeting saved. Open it from Past Meetings.")) == 1);
    CHECK_FALSE(any_contains(rig.got.statuses, "for later"));
}

// from tests/test_meeting_runtime.py::test_defer_refused_while_finalizing
TEST_CASE("test_defer_refused_while_finalizing", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_ended_meeting("m_card");
    auto probe = rig.probe();
    probe.set_finalizing(true);
    probe.set_card("m_card");
    probe.set_finalization(Json{{"status", "running"}, {"message", "Preparing…"}});

    rig.rt->defer_finalization_card();
    rig.drain();

    CHECK(rig.saved_finalization("m_card")["card_deferred"] == false);
    CHECK(any_contains(rig.got.statuses, "still being prepared"));
    CHECK(probe.card() == "m_card");
}

// from tests/test_meeting_runtime.py::test_refresh_past_meetings_from_worker_uses_signal
TEST_CASE("test_refresh_past_meetings_from_worker_uses_signal", "[test_meeting_runtime]") {
    // Opening a deferred meeting must not rebuild widgets on the worker.
    RuntimeRig rig;
    auto probe = rig.probe();
    std::thread worker([&probe] { probe.refresh_past_meetings(); });
    worker.join();
    CHECK(rig.got.refreshes == 0);
    rig.ui.run_all();
    CHECK(rig.got.refreshes == 1);
}

// from tests/test_meeting_runtime.py::test_show_past_meeting_hydrates_without_opening_browser
TEST_CASE("test_show_past_meeting_hydrates_without_opening_browser", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_ended_meeting("m_saved", true, "failed", "2025-01-02T09:30:00",
                           Json{{"ended_at", "2025-01-02T10:12:00"}, {"paused_total_s", 120}});
    auto probe = rig.probe();

    rig.rt->show_past_meeting("m_saved");
    rig.drain();

    CHECK(rig.opened_urls().empty());
    CHECK(probe.card() == "m_saved");
    REQUIRE_FALSE(rig.got.states.empty());
    const Json& state = rig.got.states.back();
    CHECK(state["meeting_id"] == "m_saved");
    CHECK_FALSE(state["display_title"].get<std::string>().empty());
    CHECK(state["started_at"] == "2025-01-02T09:30:00");
}

// from tests/test_meeting_runtime.py::test_open_report_targets_history_report_view
TEST_CASE("test_open_report_targets_history_report_view", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_ended_meeting("m_done", false, "completed");
    rig.probe().set_card("m_done");
    rig.rt->open_report();
    rig.drain();
    CHECK(rig.opened_urls() ==
          std::vector<std::string>{
              "http://127.0.0.1:9100/m/archive-token?history=m_done&view=report"});
    CHECK(rig.got.statuses.back() == messages::kOpeningReport);
}

// from tests/test_meeting_runtime.py::test_show_past_meeting_refuses_while_live
TEST_CASE("test_show_past_meeting_refuses_while_live", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.probe().set_controller_active(true);
    rig.rt->show_past_meeting("m_saved");
    rig.drain();
    CHECK(rig.opened_urls().empty());
    CHECK(rig.spawned_names().empty());
    CHECK(rig.got.statuses == std::vector<std::string>{"Finish the current meeting first."});
}

// from tests/test_meeting_runtime.py::test_open_past_meeting_reveals_without_persisting_deferral
TEST_CASE("test_open_past_meeting_reveals_without_persisting_deferral", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_ended_meeting("m_saved", true);
    auto probe = rig.probe();
    CHECK(probe.hydrate(*rig.repo->get_meeting("m_saved"), true));
    rig.drain();
    CHECK(probe.card() == "m_saved");
    REQUIRE_FALSE(rig.got.states.empty());
    CHECK(rig.got.states.back()["finalization"]["card_deferred"] == false);
    CHECK(rig.saved_finalization("m_saved")["card_deferred"] == true);
}

// from tests/test_meeting_runtime.py::test_background_handoff_keeps_work_running_and_allows_start
TEST_CASE("test_background_handoff_keeps_work_running_and_allows_start", "[test_meeting_runtime]") {
    BackgroundMeeting b;
    auto probe = b.rig.probe();
    b.handoff();
    b.handoff();
    CHECK(std::ranges::count(b.rig.spawned_names(), std::string("meeting-background-handoff")) ==
          1);
    CHECK_FALSE(probe.engine());
    CHECK_FALSE(b.rig.rt->is_finalizing());
    CHECK_FALSE(probe.card());
    CHECK(probe.background_ids() == std::vector<std::string>{"m_background"});
    CHECK(b.engine->shutdown_calls == 0);
    REQUIRE_FALSE(b.rig.got.states.empty());
    CHECK(b.rig.got.states.back()["dashboard_available"] == false);
    CHECK(b.rig.got.states.back()["background_message"].get<std::string>().find("1 meeting") !=
          std::string::npos);

    b.rig.rt->start_meeting(false);
    b.rig.drain();
    CHECK(b.rig.built.size() == 1);
    // Retained background dashboards may own the configured port.
    CHECK(b.rig.built[0].server_port == 0);
}

// from tests/test_meeting_runtime.py::test_background_handoff_waits_for_local_model_release
TEST_CASE("test_background_handoff_waits_for_local_model_release", "[test_meeting_runtime]") {
    BackgroundMeeting b;
    auto probe = b.rig.probe();
    probe.set_background_ready(false);
    b.handoff();
    CHECK(probe.engine() == b.engine);
    CHECK(b.rig.rt->is_finalizing());
    CHECK(b.rig.spawned_names().empty());
}

// from tests/test_meeting_runtime.py::test_background_handoff_persistence_failure_keeps_card
TEST_CASE("test_background_handoff_persistence_failure_keeps_card", "[test_meeting_runtime]") {
    BackgroundMeeting b(false); // no live store and no stored row: the deferral cannot persist
    auto probe = b.rig.probe();
    b.handoff();
    CHECK(probe.engine() == b.engine);
    CHECK(probe.card() == "m_background");
    CHECK(probe.background_ids().empty());
    CHECK(b.rig.got.errors ==
          std::vector<std::string>{std::string(messages::kCouldNotMoveToBackground)});
}

// from tests/test_meeting_runtime.py::test_background_outcome_does_not_replace_new_meeting
TEST_CASE("test_background_outcome_does_not_replace_new_meeting", "[test_meeting_runtime]") {
    struct Case {
        std::string status;
        Json steps;
        bool attention;
    };
    const auto c = GENERATE(
        Case{"completed", Json::array(), false}, Case{"failed", Json::array(), true},
        Case{"running", Json::array(), true},
        Case{"completed", Json::array({Json{{"id", "polish"}, {"status", "failed"}}}), true});
    BackgroundMeeting b;
    auto& rig = b.rig;
    auto probe = rig.probe();
    b.engine->set_finalization(Json{{"status", c.status}, {"steps", c.steps}});
    b.handoff();
    REQUIRE(b.threads.size() == 1);
    auto current = std::make_shared<FakeEngine>("m_new");
    probe.set_engine(current);
    probe.set_controller_active(true);
    rig.got = Recorded{};
    for (const auto& [kind, payload] : std::vector<std::pair<std::string, Json>>{
             {"status", Json{{"status", "ended"}, {"finalization", Json{{"status", "failed"}}}}},
             {"ended", Json{{"status", "ended"}}},
             {"asr_released", Json::object()},
             {"background_ready", Json::object()},
             {"error", Json{{"message", "old error"}}},
             {"server_started", Json{{"host_url", "old-url"}}}}) {
        probe.route(b.engine, kind, payload);
    }
    rig.ui.run_all();
    CHECK(rig.got.states.empty());
    CHECK(rig.got.errors.empty());
    CHECK(rig.got.servers.empty());
    CHECK(rig.lease->calls().empty());

    b.release();
    rig.ui.run_all();
    CHECK(probe.controller_active());
    CHECK(probe.engine() == current);
    CHECK_FALSE(probe.finalization());
    CHECK_FALSE(probe.card());
    REQUIRE_FALSE(rig.got.states.empty());
    for (const Json& state : rig.got.states) {
        CHECK(state.size() == 1);
        CHECK(state.contains("background_message"));
    }
    CHECK((rig.got.states.back()["background_message"].get<std::string>().find("needs attention") !=
           std::string::npos) == c.attention);
    CHECK(probe.background_ids().empty());
    CHECK(b.engine->shutdown_calls == 1);
}

// from
// tests/test_meeting_runtime.py::test_background_history_does_not_hydrate_or_delete_running_job
TEST_CASE("test_background_history_does_not_hydrate_or_delete_running_job",
          "[test_meeting_runtime]") {
    BackgroundMeeting b;
    auto& rig = b.rig;
    auto probe = rig.probe();
    b.handoff();
    rig.make_ended_meeting("m_background");
    CHECK_FALSE(probe.hydrate(*rig.repo->get_meeting("m_background"), true));
    CHECK_FALSE(probe.card());

    rig.rt->delete_past_meeting("m_background");
    rig.ui.run_all();
    CHECK(std::ranges::count(rig.spawned_names(), std::string("meeting-delete")) == 0);
    CHECK(rig.got.statuses.back() == messages::kStillInBackground);
    probe.delete_past_meeting_worker("m_background");
    probe.clear_past_meetings_worker(true);
    rig.ui.run_all();
    CHECK(rig.repo->get_meeting("m_background").has_value());
}

// from tests/test_meeting_runtime.py::test_events_queued_before_background_handoff_are_discarded
TEST_CASE("test_events_queued_before_background_handoff_are_discarded", "[test_meeting_runtime]") {
    BackgroundMeeting b;
    auto& rig = b.rig;
    auto probe = rig.probe();
    rig.rt->continue_in_background();
    rig.workers.run_all(); // the deferral is persisted; the handoff waits on the UI actor
    std::thread queued([&b] { b.engine->emit("ended", Json{{"status", "ended"}}); });
    queued.join();
    REQUIRE(rig.ui.run_one()); // the handoff
    auto current = std::make_shared<FakeEngine>("m_new");
    probe.set_engine(current);
    probe.set_controller_active(true);
    rig.got = Recorded{};
    rig.ui.run_all();
    CHECK(probe.controller_active());
    CHECK(rig.got.states.empty());
}

// from tests/test_meeting_runtime.py::test_open_background_meeting_reuses_its_processing_dashboard
TEST_CASE("test_open_background_meeting_reuses_its_processing_dashboard",
          "[test_meeting_runtime]") {
    BackgroundMeeting b;
    auto& rig = b.rig;
    auto probe = rig.probe();
    probe.set_host_url("http://127.0.0.1:8765/h/background");
    b.handoff();
    rig.make_ended_meeting("m_background");
    probe.open_past_meeting_worker("m_background");
    CHECK(rig.opened_urls() == std::vector<std::string>{"http://127.0.0.1:8765/h/background"});
    CHECK_FALSE(probe.archive());
    CHECK_FALSE(probe.card());
}
