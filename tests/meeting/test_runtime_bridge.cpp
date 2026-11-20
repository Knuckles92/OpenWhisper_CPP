// tests/test_meeting_runtime.py: engine events reaching the desktop (ended, status,
// finalization, the dashboard-only events) and the live dashboard link.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/log.h"
#include "meeting/runtime_messages.h"
#include "runtime_support.h"

using namespace fv::meeting;
using namespace fv::meeting::runtime_test;
using fv::Json;

namespace {

// Every log line of the runtime's logger, debug included, while it lives.
class LogCapture final : public fv::log::Sink {
public:
    LogCapture() { fv::log::set_logger_level("services.runtime.meeting", fv::log::Level::Debug); }
    void write(const fv::log::Record& record) override {
        if (record.logger == "services.runtime.meeting") {
            const std::lock_guard lock(mutex_);
            lines_.emplace_back(record.message);
        }
    }
    bool contains(std::string_view text) const {
        const std::lock_guard lock(mutex_);
        return std::ranges::any_of(lines_, [text](const std::string& line) {
            return line.find(text) != std::string::npos;
        });
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

struct ScopedCapture {
    ScopedCapture() : capture(std::make_shared<LogCapture>()) { fv::log::add_sink(capture); }
    ~ScopedCapture() {
        fv::log::remove_sink(capture);
        fv::log::set_logger_level("services.runtime.meeting", std::nullopt);
    }
    std::shared_ptr<LogCapture> capture;
};

} // namespace

// from tests/test_meeting_runtime.py::test_open_dashboard_without_url_surfaces_error
TEST_CASE("test_open_dashboard_without_url_surfaces_error", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.rt->open_dashboard();
    rig.drain();
    CHECK(rig.got.errors == std::vector<std::string>{std::string(messages::kNoDashboard)});
    CHECK(rig.got.statuses.empty());
    CHECK(rig.opened_urls().empty());
}

// from tests/test_meeting_runtime.py::test_open_dashboard_for_retained_card_uses_archive_path
TEST_CASE("test_open_dashboard_for_retained_card_uses_archive_path", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_ended_meeting("m_saved", false, "completed");
    rig.probe().set_card("m_saved");
    rig.rt->open_dashboard();
    rig.drain();
    // The card's own meeting opens through the past-meeting path (the archive dashboard).
    CHECK(std::ranges::count(rig.spawned_names(), std::string("meeting-history-dashboard")) == 1);
    CHECK(rig.opened_urls() ==
          std::vector<std::string>{"http://127.0.0.1:9100/m/archive-token?history=m_saved"});
}

// from tests/test_meeting_runtime.py::test_ended_unlocks_active_while_retaining_dashboard
TEST_CASE("test_ended_unlocks_active_while_retaining_dashboard", "[test_meeting_runtime]") {
    RuntimeRig rig;
    auto probe = rig.probe();
    probe.set_host_url("http://127.0.0.1:8765/host");
    probe.set_finalizing(true);
    probe.set_finalization(Json{{"status", "running"}, {"message", "Preparing final insights…"}});
    probe.set_controller_active(true);

    probe.on_engine_event("ended", Json{{"status", "ended"}, {"meeting_id", "m1"}});
    rig.drain();

    CHECK_FALSE(probe.controller_active());
    CHECK_FALSE(rig.rt->is_claimed());
    CHECK(probe.host_url() == "http://127.0.0.1:8765/host");
    REQUIRE_FALSE(rig.got.states.empty());
    CHECK(rig.got.states.back()["active"] == false);
    CHECK(rig.got.states.back()["finalization"]["status"] == "running");
    CHECK(rig.got.statuses.back() == messages::kEndedFinalizing);
}

// from tests/test_meeting_runtime.py::test_terminal_finalization_clears_guard_without_modal
TEST_CASE("test_terminal_finalization_clears_guard_without_modal", "[test_meeting_runtime]") {
    RuntimeRig rig;
    auto probe = rig.probe();
    probe.set_finalizing(true);
    probe.set_finalization(Json{{"status", "running"}, {"message", "…"}});
    probe.set_controller_active(false);

    probe.on_engine_event(
        "status", Json{{"status", "ended"},
                       {"finalization",
                        Json{{"status", "completed"}, {"message", "Final insights are ready."}}}});
    rig.drain();

    CHECK_FALSE(rig.rt->is_finalizing());
    CHECK(probe.finalization().value()["status"] == "completed");
    CHECK(rig.got.errors.empty());
    CHECK(std::ranges::any_of(rig.got.states, [](const Json& state) {
        return state.contains("finalization") && state["finalization"].is_object() &&
               state["finalization"]["status"] == "completed";
    }));
}

// from tests/test_meeting_runtime.py::test_engine_pause_status_mirrors_onto_the_tab
TEST_CASE("test_engine_pause_status_mirrors_onto_the_tab", "[test_meeting_runtime]") {
    RuntimeRig rig;
    auto probe = rig.probe();
    auto engine = std::make_shared<FakeEngine>();
    probe.set_engine(engine);

    engine->time_s = 12.5;
    probe.on_engine_event("status", Json{{"status", "paused"}});
    probe.on_engine_event("status", Json{{"status", "active"}});
    probe.on_engine_event("status", Json{{"status", "ending"}});
    rig.drain();

    const auto& states = rig.got.states;
    REQUIRE(states.size() == 3);
    CHECK(states[0]["paused"] == true);
    CHECK(states[1]["paused"] == false);
    CHECK_FALSE(states[2].contains("paused"));
    // Dashboard pause and resume carry the engine clock, so the tab neither resets nor counts
    // the paused span.
    for (int i = 0; i < 2; ++i) {
        CHECK(states[i].contains("elapsed_s"));
        CHECK(states[i].contains("elapsed_at"));
        CHECK(states[i]["elapsed_s"] == 12.5);
    }
    CHECK_FALSE(states[2].contains("elapsed_s"));
}

// from tests/test_meeting_runtime.py::test_failed_finalization_is_non_modal
TEST_CASE("test_failed_finalization_is_non_modal", "[test_meeting_runtime]") {
    RuntimeRig rig;
    auto probe = rig.probe();
    probe.on_engine_event(
        "status", Json{{"status", "ended"},
                       {"finalization",
                        Json{{"status", "failed"}, {"message", "Final insights failed: boom"}}}});
    rig.drain();
    CHECK(rig.got.errors.empty());
    CHECK(probe.finalization().value()["status"] == "failed");
    CHECK_FALSE(rig.rt->is_finalizing());
    CHECK(rig.got.statuses.back() == "Final insights failed: boom");
}

// from tests/test_meeting_runtime.py::test_voice_feedback_reaches_no_desktop_surface
TEST_CASE("test_voice_feedback_reaches_no_desktop_surface", "[test_meeting_runtime]") {
    ScopedCapture logs;
    RuntimeRig rig;
    auto probe = rig.probe();
    auto current = std::make_shared<FakeEngine>();
    probe.set_engine(current);
    probe.route(current, "voice_feedback",
                Json{{"seq", 1}, {"phase", "heard"}, {"message", "Heard you"}});
    rig.drain();
    CHECK(rig.got.states.empty());
    CHECK(rig.got.statuses.empty());
    CHECK(rig.got.errors.empty());
    // Handled deliberately, not by falling through to the unknown-event branch.
    CHECK_FALSE(logs.capture->contains("Unhandled meeting engine event"));

    probe.route(current, "mystery", Json::object());
    CHECK(logs.capture->contains("Unhandled meeting engine event: mystery"));
}

TEST_CASE("engine events reach the desktop only from the current engine, then go public",
          "[meeting][runtime]") {
    RuntimeRig rig;
    auto probe = rig.probe();
    auto current = std::make_shared<FakeEngine>("m_now");
    auto old = std::make_shared<FakeEngine>("m_old");
    probe.set_engine(old, true);
    probe.set_engine(current, true);
    old->emit("server_started", Json{{"host_url", "old"}});

    // Queued on the engine's own thread, routed on the UI actor.
    current->emit("server_started", Json{{"host_url", "http://127.0.0.1:8765/m/h"},
                                         {"guest_url", "http://127.0.0.1:8765/m/g"}});
    current->emit("intelligence", Json{{"online", false}});
    rig.drain();
    CHECK(rig.got.servers.size() == 1);
    CHECK(probe.host_url() == "http://127.0.0.1:8765/m/h");
    CHECK(rig.got.statuses.back() == messages::kIntelligenceOffline);
    CHECK(rig.got.engine_events == std::vector<std::pair<std::string, std::string>>{
                                       {"m_now", "server_started"}, {"m_now", "intelligence"}});

    rig.rt->copy_guest_link();
    rig.drain();
    CHECK(rig.got.guest_links == std::vector<std::string>{"http://127.0.0.1:8765/m/g"});
}

TEST_CASE("an error event is shown once the start is over", "[meeting][runtime]") {
    RuntimeRig rig;
    auto probe = rig.probe();
    probe.on_engine_event("error", Json{{"code", "x"}});
    rig.drain();
    // Without a message the whole payload is shown, as str(dict) prints it.
    CHECK(rig.got.errors == std::vector<std::string>{"{'code': 'x'}"});
    rig.rt->copy_guest_link();
    rig.drain();
    CHECK(rig.got.statuses.back() == messages::kNoGuestLink);
}
