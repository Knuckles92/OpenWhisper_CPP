// The application controller (services/application_controller.py) over the real feature graph:
// engine switches and reloads, the meeting gates, the engine lease, the deferred startup
// database and Quit. Ported from tests/test_application_controller.py; where Python counted
// executor submissions, these count the fake engine's loads.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "appcore/cancel_router.h"
#include "appcore/shutdown_plan.h"
#include "base/testing/manual_executor.h"
#include "controller_support.h"
#include "history/manager.h"
#include "hotkeys/hotkey_manager.h"
#include "settings/keys.h"
#include "ui_record/compact_record_controller.h"
#include "ui_widgets/buttons.h"

using fvtest::ControllerRig;
using fvtest::RigOptions;
using fvtest::waitUntil;
using namespace std::chrono_literals;

namespace {

const std::string kMeetingRefusal = "Meeting Mode is active — end the meeting to use dictation";
const std::string kEngineRefusal = "End the meeting before changing the engine";

std::size_t loads(fv::speech::FakeEngine& engine) {
    const auto ops = engine.ops();
    return static_cast<std::size_t>(std::ranges::count(ops, std::string("load")));
}

// What a signal carried, in order, received on the UI executor.
template <class T> struct Seen {
    std::vector<T> values;
    fv::Subscription link;
};

} // namespace

// from
// tests/test_application_controller.py::TestApplicationController::test_model_switch_updates_backend_and_device_info
TEST_CASE("test_model_switch_updates_backend_and_device_info",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    std::vector<std::string> deviceInfos;
    auto link = rig.runtime().device_info_changed.connect(
        rig.uiExecutor(),
        [&deviceInfos](std::string info, std::optional<bool>) { deviceInfos.push_back(info); });

    rig.app().on_model_changed("api");
    CHECK(rig.settings().value("selected_model") == "api");
    REQUIRE_FALSE(deviceInfos.empty());
    CHECK(deviceInfos.back().empty());

    rig.app().on_model_changed("parakeet");
    CHECK(rig.settings().value("selected_model") == "parakeet");
    // Switching to the local engine reloads it.
    CHECK(waitUntil(
        [&rig] { return loads(rig.engine()) == 1 && rig.app().reload_machine().settled(); }));
}

// from
// tests/test_application_controller.py::TestApplicationController::test_reload_whisper_model_runs_in_background_and_reports
// (the Parakeet engine through the speech service)
TEST_CASE("test_reload_whisper_model_runs_in_background_and_reports",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    std::vector<bool> busy;
    std::vector<std::string> deviceInfos;
    auto busyLink = rig.app().engine_busy_changed.connect(
        rig.uiExecutor(), [&busy](bool value) { busy.push_back(value); });
    auto infoLink = rig.app().device_info_update.connect(
        rig.uiExecutor(),
        [&deviceInfos](std::string info, std::optional<bool>) { deviceInfos.push_back(info); });

    // Scheduling only arms the debounce timer; nothing runs yet.
    rig.app().reload_whisper_model();
    CHECK(loads(rig.engine()) == 0);
    CHECK_FALSE(rig.app().reload_machine().settled());

    // The debounce fires, the engine goes busy and loads off the GUI thread, then reports.
    REQUIRE(waitUntil([&rig, &busy] {
        return loads(rig.engine()) == 1 && rig.app().reload_machine().settled() && !busy.empty() &&
               !busy.back();
    }));
    CHECK(busy.front());
    REQUIRE_FALSE(deviceInfos.empty());
    CHECK(deviceInfos.back() == "Parakeet TDT 0.6B v3 | cpu");
    CHECK(rig.hasStatus("Reloading speech engine..."));
    CHECK(rig.hasStatus("Parakeet TDT 0.6B v3 | cpu"));
}

// from
// tests/test_application_controller.py::TestApplicationController::test_reload_whisper_model_refused_while_recording
TEST_CASE("test_reload_whisper_model_refused_while_recording",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    rig.activity().set_recording(true);

    rig.app().reload_whisper_model();

    QApplication::processEvents();
    CHECK(rig.hasStatus("Finish recording before changing the engine"));
    CHECK(rig.app().reload_machine().settled());
    CHECK(loads(rig.engine()) == 0);
    rig.activity().set_recording(false);
}

// from
// tests/test_application_controller.py::TestApplicationController::test_hotkeys_backfill_minimize_tray_and_refresh_display_on_update
TEST_CASE("test_hotkeys_backfill_minimize_tray_and_refresh_display_on_update",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    auto& manager = rig.context().service<fv::hotkeys::HotkeyManager>();
    CHECK(manager.hotkeys().at("minimize_tray") == "ctrl+alt+m");
    // What the UI is told to show, as the controller's own connection hears it.
    std::vector<fv::dictation::HotkeyRuntime::HotkeyMap> shown;
    auto link = rig.hotkeys().hotkeys_changed.connect(
        rig.uiExecutor(),
        [&shown](fv::dictation::HotkeyRuntime::HotkeyMap map) { shown.push_back(map); });

    const fv::hotkeys::HotkeyList updated{{"record_toggle", "f4"},
                                          {"cancel", "f5"},
                                          {"enable_disable", "f6"},
                                          {"minimize_tray", "ctrl+alt+h"}};
    rig.app().update_hotkeys(updated);
    QApplication::processEvents();

    const auto now = manager.hotkeys();
    for (const auto& [action, hotkey] : updated) {
        CHECK(now.at(action) == hotkey);
    }
    const fv::Json saved = rig.settings().value("hotkeys");
    for (const auto& [action, hotkey] : updated) {
        CHECK(saved.value(action, std::string()) == hotkey);
    }
    REQUIRE(shown.size() == 1);
    for (const auto& [action, hotkey] : updated) {
        CHECK(shown.back().at(action) == hotkey);
    }
    CHECK(rig.hasStatus("Hotkeys updated"));
}

// from
// tests/test_application_controller.py::TestApplicationController::test_minimize_hotkey_toggles_tray_visibility_on_main_thread
TEST_CASE("test_minimize_hotkey_toggles_tray_visibility_on_main_thread",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    rig.window().show();
    REQUIRE(rig.window().isVisible());

    // The hotkey's thread only asks; the window changes on the GUI thread.
    std::thread([&rig] { rig.app().minimize_to_tray(); }).join();
    CHECK(rig.window().isVisible());
    CHECK(waitUntil([&rig] { return !rig.window().isVisible(); }));
}

// from
// tests/test_application_controller.py::TestApplicationController::test_dictation_start_is_refused_and_rolled_back_during_a_meeting
TEST_CASE("test_dictation_start_is_refused_and_rolled_back_during_a_meeting",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    rig.activity().set_meeting_active(true);

    CHECK_FALSE(rig.ui().startRecording());

    CHECK_FALSE(rig.runtime().is_recording());
    CHECK_FALSE(rig.ui().isRecording());
    CHECK_FALSE(rig.window().isRecording());
    // The compact controller was told the recording chrome went back.
    CHECK(rig.ui().compactController()->statusLabel()->text() == QStringLiteral("Ready to record"));
    CHECK(rig.hasStatus(kMeetingRefusal));
    rig.activity().set_meeting_active(false);
}

// from
// tests/test_application_controller.py::TestApplicationController::test_recording_starts_normally_without_a_meeting
TEST_CASE("test_recording_starts_normally_without_a_meeting",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());

    CHECK(rig.ui().startRecording());

    CHECK(rig.runtime().is_recording());
    CHECK(waitUntil([&rig] { return rig.window().isRecording(); }));
    rig.ui().cancelRecording();
    CHECK(waitUntil([&rig] { return !rig.runtime().is_recording(); }));
}

// from
// tests/test_application_controller.py::TestApplicationController::test_dictation_is_refused_while_meeting_setup_owns_audio
TEST_CASE("test_dictation_is_refused_while_meeting_setup_owns_audio",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    rig.activity().set_meeting_setup_open(true);

    CHECK_FALSE(rig.ui().startRecording());

    CHECK_FALSE(rig.runtime().is_recording());
    CHECK_FALSE(rig.ui().isRecording());
    CHECK(rig.hasStatus("Close Meeting Mode setup before using Quick Record."));
    rig.activity().set_meeting_setup_open(false);
}

// from
// tests/test_application_controller.py::TestApplicationController::test_toggle_recording_is_refused_during_a_meeting
TEST_CASE("test_toggle_recording_is_refused_during_a_meeting",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    rig.activity().set_meeting_active(true);

    CHECK_FALSE(rig.app().toggle_recording());

    CHECK_FALSE(rig.runtime().is_recording());
    CHECK(rig.hasStatus(kMeetingRefusal));
    rig.activity().set_meeting_active(false);
}

// from
// tests/test_application_controller.py::TestApplicationController::test_whisper_reload_is_refused_during_a_meeting
// (the Parakeet engine)
TEST_CASE("test_whisper_reload_is_refused_during_a_meeting",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    rig.activity().set_meeting_active(true);

    rig.app().reload_whisper_model();

    QApplication::processEvents();
    CHECK(rig.hasStatus(kEngineRefusal));
    CHECK(loads(rig.engine()) == 0);
    rig.activity().set_meeting_active(false);
}

// from
// tests/test_application_controller.py::TestApplicationController::test_release_frees_the_local_model_and_owes_a_reload
TEST_CASE("test_release_frees_the_local_model_and_owes_a_reload",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());

    CHECK(rig.app().release_local_engine());

    CHECK(waitUntil([&rig] {
        return rig.speech().readiness(fv::speech::Slot::Dictation).state ==
               fv::speech::ReadyState::Offline;
    }));
    CHECK(rig.app().reload_machine().released_for_lease());
    CHECK(loads(rig.engine()) == 1);
}

// from
// tests/test_application_controller.py::TestApplicationController::test_release_is_a_noop_when_no_model_is_loaded
TEST_CASE("test_release_is_a_noop_when_no_model_is_loaded",
          "[test_application_controller][widget]") {
    ControllerRig rig;

    CHECK_FALSE(rig.app().release_local_engine());

    CHECK_FALSE(rig.app().reload_machine().released_for_lease());
}

// from
// tests/test_application_controller.py::TestApplicationController::test_restore_reloads_only_after_a_release
TEST_CASE("test_restore_reloads_only_after_a_release", "[test_application_controller][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());

    rig.app().restore_local_engine();
    QApplication::processEvents();
    CHECK(loads(rig.engine()) == 1);

    rig.app().release_local_engine();
    rig.app().restore_local_engine();

    CHECK(waitUntil(
        [&rig] { return loads(rig.engine()) == 2 && rig.app().reload_machine().settled(); }));
    CHECK_FALSE(rig.app().reload_machine().released_for_lease());
}

// from tests/test_application_controller.py::TestApplicationController::test_restore_is_idempotent
TEST_CASE("test_restore_is_idempotent", "[test_application_controller][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    rig.app().release_local_engine();

    rig.app().restore_local_engine();
    rig.app().restore_local_engine();

    REQUIRE(waitUntil(
        [&rig] { return rig.app().reload_machine().settled() && loads(rig.engine()) >= 2; }));
    fvtest::waitUntil([] { return false; }, 200ms);
    CHECK(loads(rig.engine()) == 2);
}

// from
// tests/test_application_controller.py::TestApplicationController::test_repeated_lease_release_keeps_restore_obligation
TEST_CASE("test_repeated_lease_release_keeps_restore_obligation",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    CHECK(rig.app().release_local_engine());
    // Nothing is loaded now, so a second release frees nothing, and the first one's restore is
    // still owed.
    CHECK_FALSE(rig.app().release_local_engine());
    CHECK(rig.app().reload_machine().released_for_lease());

    rig.app().restore_local_engine();
    CHECK(waitUntil(
        [&rig] { return loads(rig.engine()) == 2 && rig.app().reload_machine().settled(); }));
}

// A lease taken while the engine loads waits for the load, then frees what it brought up, with
// no warm-up on the closed engine; the restore loads it again.
TEST_CASE("a lease during the load releases the engine that load brings up",
          "[test_application_controller][widget]") {
    RigOptions options;
    options.script.load_delay = 300ms;
    ControllerRig rig(options);
    rig.mainWindowShown();
    REQUIRE(waitUntil([&rig] { return loads(rig.engine()) == 1; }));
    REQUIRE_FALSE(rig.app().reload_machine().settled());

    CHECK(rig.app().release_local_engine());

    REQUIRE(waitUntil([&rig] { return rig.app().reload_machine().settled(); }));
    CHECK(waitUntil([&rig] {
        return rig.speech().readiness(fv::speech::Slot::Dictation).state ==
               fv::speech::ReadyState::Offline;
    }));
    CHECK(rig.engine().wait_calls(2, 2s));
    const auto ops = rig.engine().ops();
    CHECK(std::ranges::count(ops, std::string("release")) == 1);
    CHECK(std::ranges::count(ops, std::string("warm_up")) == 0);
    CHECK(rig.app().reload_machine().released_for_lease());

    rig.app().restore_local_engine();
    CHECK(waitUntil([&rig] {
        return loads(rig.engine()) == 2 && rig.app().reload_machine().settled() &&
               rig.speech().readiness(fv::speech::Slot::Dictation).state ==
                   fv::speech::ReadyState::Ready;
    }));
}

// A lease taken during the warm-up closes the engine the warm-up runs on (after it, on the
// speech thread), and the reload still settles.
TEST_CASE("a lease during the warm-up releases the warmed engine",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    rig.engine().hold();
    rig.mainWindowShown();
    // The load's answer reaches the GUI thread before the warm-up starts, so the event loop turns.
    REQUIRE(waitUntil([&rig] { return rig.engine().wait_entered(1ms); }));
    REQUIRE(rig.engine().ops().back() == "warm_up");

    CHECK(rig.app().release_local_engine());
    rig.engine().resume();

    REQUIRE(waitUntil([&rig] { return rig.app().reload_machine().settled(); }));
    CHECK(waitUntil([&rig] {
        const auto ops = rig.engine().ops();
        return !ops.empty() && ops.back() == "release";
    }));
    CHECK(rig.speech().readiness(fv::speech::Slot::Dictation).state ==
          fv::speech::ReadyState::Offline);
    CHECK(rig.app().reload_machine().released_for_lease());
}

// The release's Offline, still on its way to the UI thread when the restore starts the next
// load, is not taken for that load's answer: the engine comes back ready, and nothing offers a
// runtime that is installed.
TEST_CASE("a restore right after a release reloads the engine and offers no runtime",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    std::vector<std::string> offered;
    auto link = rig.app().runtime_consent_requested.connect(
        rig.uiExecutor(), [&offered](std::string model) { offered.push_back(model); });
    std::vector<std::optional<bool>> usable;
    auto infoLink = rig.app().device_info_update.connect(
        rig.uiExecutor(),
        [&usable](std::string, std::optional<bool> ready) { usable.push_back(ready); });

    REQUIRE(rig.app().release_local_engine());
    rig.app().restore_local_engine();

    REQUIRE(waitUntil([&rig] {
        return loads(rig.engine()) == 2 && rig.app().reload_machine().settled() &&
               rig.speech().readiness(fv::speech::Slot::Dictation).state ==
                   fv::speech::ReadyState::Ready;
    }));
    fvtest::waitUntil([] { return false; }, 200ms);
    CHECK(offered.empty());
    REQUIRE_FALSE(usable.empty());
    CHECK(usable.back() == std::optional<bool>(true));
}

// from
// tests/test_application_controller.py::TestApplicationController::test_cleanup_is_safe_with_partial_state
TEST_CASE("test_cleanup_is_safe_with_partial_state", "[test_application_controller][widget]") {
    ControllerRig rig;
    // Quit before the window was ever shown: no engine, no database step, no tray click.
    rig.plan().run();

    CHECK(rig.app().shutting_down());
    CHECK(rig.activity().snapshot().shutting_down);
    CHECK_FALSE(rig.overlay().isVisible());
    // The database closed last.
    CHECK_THROWS_AS(fvtest::offGui([&rig] {
                        return rig.database().read([](fv::sql::Connection& connection) {
                            return connection.scalar("SELECT 1");
                        });
                    }),
                    fv::sql::Error);
}

// from
// tests/test_application_controller.py::TestApplicationController::test_notify_main_ui_ready_loads_deferred_local_backend
TEST_CASE("test_notify_main_ui_ready_loads_deferred_local_backend",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    std::vector<bool> busy;
    auto link = rig.app().engine_busy_changed.connect(
        rig.uiExecutor(), [&busy](bool value) { busy.push_back(value); });
    CHECK(loads(rig.engine()) == 0);

    REQUIRE(rig.startEngine());

    std::string seen;
    for (const auto& status : rig.statuses()) {
        seen += status + " | ";
    }
    for (const auto& op : rig.engine().ops()) {
        seen += op + ", ";
    }
    INFO(seen);
    CHECK(rig.hasStatus("Loading speech engine..."));
    REQUIRE_FALSE(busy.empty());
    CHECK(busy.front());
    CHECK_FALSE(busy.back());
    CHECK(loads(rig.engine()) == 1);
    CHECK(rig.hasStatus("Parakeet TDT 0.6B v3 | cpu"));
    // The warm-up ran before the engine counted as settled.
    const auto ops = rig.engine().ops();
    CHECK(std::ranges::count(ops, std::string("warm_up")) == 1);
}

namespace {

// An ApplicationController of its own over the rig's runtime and engine, with a scripted
// startup database step and recovery, on executors the test runs.
struct StandaloneController {
    explicit StandaloneController(ControllerRig& rig) {
        fv::ApplicationServices services;
        services.settings = &rig.settings();
        services.activity = &activity;
        services.speech = &rig.speech();
        services.transcription = &rig.runtime();
        services.cancel = &cancel;
        services.ui = &rig.uiExecutor();
        services.startup = &startup;
        services.jobs = &jobs;
        services.resolve_engine = [](const fv::Json&) {
            return fv::speech::BackendConfig{};
        };
        services.ensure_database = [this] {
            ensure();
        };
        services.recover_recordings = [this] {
            calls.add("recover");
        };
        app = std::make_unique<fv::ApplicationController>(std::move(services));
        statusLink = app->status_update.connect(
            rig.uiExecutor(), [this](std::string text) { statuses.push_back(text); });
    }

    std::function<void()> ensure = [] {
    };
    fvtest::CallLog calls;
    std::vector<std::string> statuses;
    fv::ActivityState activity;
    fv::CancelRouter cancel;
    fv::ManualExecutor startup;
    fv::ManualExecutor jobs;
    std::unique_ptr<fv::ApplicationController> app;
    fv::Subscription statusLink;
};

} // namespace

// from
// tests/test_application_controller.py::TestApplicationController::test_startup_database_is_deferred_and_dependencies_wait_for_completion
TEST_CASE("test_startup_database_is_deferred_and_dependencies_wait_for_completion",
          "[test_application_controller][widget]") {
    ControllerRig rig(RigOptions{.settings = fv::Json{{"selected_model", "api"}}});
    StandaloneController standalone(rig);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    standalone.ensure = [&] {
        CHECK_FALSE(fv::is_gui_thread());
        entered.set_value();
        REQUIRE(released.wait_for(3s) == std::future_status::ready);
        standalone.calls.add("database");
    };

    standalone.app->notify_main_ui_ready();
    standalone.app->notify_main_ui_ready();
    CHECK(standalone.calls.all().empty());

    // One step was submitted; it runs on a thread of its own.
    std::thread worker([&standalone] { CHECK(standalone.startup.run_all() == 1); });
    REQUIRE(entered.get_future().wait_for(3s) == std::future_status::ready);
    CHECK(standalone.calls.all().empty());
    release.set_value();
    worker.join();

    // The dependents start once the answer reaches the UI thread.
    REQUIRE(waitUntil([&standalone] { return standalone.app->startup_database_ready(); }));
    std::thread([&standalone] { standalone.jobs.run_all(); }).join();
    CHECK(standalone.calls.all() == std::vector<std::string>{"database", "recover"});
    CHECK(standalone.activity.snapshot().startup_ready);
    CHECK_FALSE(standalone.activity.snapshot().recovery_running);
}

// from
// tests/test_application_controller.py::TestApplicationController::test_startup_database_failure_or_shutdown_does_not_start_dependents
TEST_CASE("test_startup_database_failure_or_shutdown_does_not_start_dependents",
          "[test_application_controller][widget]") {
    const bool closing = GENERATE(false, true);
    ControllerRig rig(RigOptions{.settings = fv::Json{{"selected_model", "api"}}});
    StandaloneController standalone(rig);
    standalone.ensure = [] {
        throw std::runtime_error("database unavailable");
    };
    fv::ShutdownPlan plan;
    if (closing) {
        standalone.app->add_shutdown_steps(plan);
        plan.run();
    }

    std::thread([&standalone] { standalone.app->initialize_startup_database(); }).join();
    fvtest::waitUntil([] { return false; }, 100ms);
    std::thread([&standalone] { standalone.jobs.run_all(); }).join();

    CHECK(standalone.calls.all().empty());
    CHECK_FALSE(standalone.app->startup_database_ready());
    if (!closing) {
        CHECK(standalone.app->update_readiness_error() == "database unavailable");
        CHECK(std::ranges::count(standalone.statuses, std::string("database unavailable")) == 1);
    }
}

// from
// tests/test_application_controller.py::TestApplicationController::test_start_recording_refused_while_local_engine_loading
TEST_CASE("test_start_recording_refused_while_local_engine_loading",
          "[test_application_controller][widget]") {
    RigOptions options;
    options.script.load_delay = 1500ms;
    ControllerRig rig(options);
    rig.mainWindowShown();
    REQUIRE(waitUntil([&rig] { return rig.app().reload_machine().in_flight(); }));

    CHECK_FALSE(rig.app().start_recording());

    CHECK(rig.statuses().back() == "Speech engine is still loading...");
    CHECK_FALSE(rig.runtime().is_recording());
    CHECK(waitUntil([&rig] { return rig.app().reload_machine().settled(); }));
}

// from
// tests/test_application_controller.py::TestApplicationController::test_start_recording_prompts_for_missing_model_before_capture
TEST_CASE("test_start_recording_prompts_for_missing_model_before_capture",
          "[test_application_controller][widget]") {
    RigOptions options;
    options.engineInstalled = false;
    options.setupAnswer = false;
    options.script.readiness = fv::speech::Readiness{fv::speech::ReadyState::ModelMissing,
                                                     "Download Parakeet TDT 0.6B v3 in Downloads.",
                                                     "Download Parakeet TDT 0.6B v3 in Downloads."};
    ControllerRig rig(options);
    rig.mainWindowShown();
    // The setup dialog was offered once at startup, as the model is missing.
    REQUIRE(waitUntil([&rig] {
        return rig.app().reload_machine().settled() &&
               rig.speech().readiness(fv::speech::Slot::Dictation).state ==
                   fv::speech::ReadyState::ModelMissing;
    }));
    const auto offered = [&rig] {
        const auto calls = rig.calls().all();
        return std::ranges::count(calls, std::string("ensure_engine"));
    };
    const auto before = offered();
    CHECK(before >= 1);

    CHECK_FALSE(rig.app().start_recording());

    CHECK(waitUntil([&] { return offered() == before + 1; }));
    CHECK_FALSE(rig.runtime().is_recording());
    CHECK(rig.statuses().back().find("Download") != std::string::npos);
}

// from
// tests/test_application_controller.py::TestApplicationController::test_optional_selection_cannot_change_while_a_job_is_active
TEST_CASE("test_optional_selection_cannot_change_while_a_job_is_active",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    REQUIRE(rig.runtime().claim_job());

    rig.app().on_model_changed("api");

    CHECK(rig.settings().value("selected_model", fv::Json("parakeet")) == "parakeet");
    CHECK(rig.hasStatus("Finish recording or transcription before changing engines"));
    rig.runtime().finish_job();
}

// from
// tests/test_application_controller.py::TestApplicationController::test_quit_drains_queued_history_before_closing_database
TEST_CASE("test_quit_drains_queued_history_before_closing_database",
          "[test_application_controller][widget]") {
    ControllerRig rig(RigOptions{.settings = fv::Json{{"auto_paste", false}}});
    // A save already running holds the one persistence worker.
    std::promise<void> entered;
    std::atomic<bool> release{false};
    rig.context().persistence().post([&] {
        entered.set_value();
        while (!release) {
            std::this_thread::sleep_for(5ms);
        }
    });
    REQUIRE(entered.get_future().wait_for(2s) == std::future_status::ready);
    REQUIRE(rig.runtime().claim_job());
    rig.runtime().on_transcription_complete("accepted words survive Quit");
    auto& history = rig.context().service<fv::HistoryManager>();
    CHECK(fvtest::offGui([&history] { return history.get_history().size(); }) == 0);

    std::vector<std::string> savedAtDatabaseClose;
    rig.plan().add(fv::ShutdownStep::UiController, "test", [&] {
        savedAtDatabaseClose = fvtest::offGui([&history] {
            std::vector<std::string> texts;
            for (const auto& entry : history.get_history()) {
                texts.push_back(entry.text);
            }
            return texts;
        });
    });
    // Released only once Quit began, so the save was still queued when Quit reached the drain.
    std::thread releaser([&rig, &release] {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!rig.app().shutting_down() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        release = true;
    });
    rig.plan().run();
    releaser.join();

    CHECK(savedAtDatabaseClose == std::vector<std::string>{"accepted words survive Quit"});
}

// ---- M2: the API engine ----

// from
// tests/test_application_controller.py::TestApplicationController::test_notify_main_ui_ready_skips_local_load_for_api_backend
TEST_CASE("test_notify_main_ui_ready_skips_local_load_for_api_backend",
          "[test_application_controller][widget]") {
    RigOptions options;
    options.settings = fv::Json{{"selected_model", "api"}};
    ControllerRig rig(options);

    rig.mainWindowShown();
    REQUIRE(waitUntil([&rig] { return rig.app().startup_database_ready(); }));

    // No reload ran, and nothing loaded a local model: the API engine went into the slot as it
    // is, so its readiness says whether a key resolves.
    CHECK_FALSE(rig.app().reload_machine().in_flight());
    CHECK(rig.app().reload_machine().settled());
    for (const fv::speech::FakeCall& call : rig.engine().calls()) {
        if (call.op == "load") {
            CHECK(call.model_key.empty());
        }
    }
    // The controller hears the slot's Ready on the UI thread after the speech service has it,
    // so what it says is loaded is waited for too.
    CHECK(waitUntil([&rig] {
        return rig.speech().readiness(fv::speech::Slot::Dictation).state ==
                   fv::speech::ReadyState::Ready &&
               rig.app().loaded_engine().has_value();
    }));
    const auto loaded = rig.app().loaded_engine();
    REQUIRE(loaded.has_value());
    CHECK(loaded->backend == "api");
    CHECK(rig.app().loaded_runtime_dir().empty());
}

// from
// tests/test_application_controller.py::TestApplicationController::test_start_recording_refuses_unavailable_api_before_capture
TEST_CASE("test_start_recording_refuses_unavailable_api_before_capture",
          "[test_application_controller][widget]") {
    RigOptions options;
    options.settings = fv::Json{{"selected_model", "api"}};
    options.script.readiness = {fv::speech::ReadyState::NeedsKey,
                                "The selected API transcription engine needs a valid API key",
                                "The selected API transcription engine needs a valid API key"};
    ControllerRig rig(options);
    rig.mainWindowShown();
    REQUIRE(waitUntil([&rig] {
        return rig.speech().readiness(fv::speech::Slot::Dictation).state ==
               fv::speech::ReadyState::NeedsKey;
    }));

    CHECK_FALSE(rig.app().start_recording());

    CHECK_FALSE(rig.runtime().is_recording());
    CHECK(rig.audio().streams().empty());
    REQUIRE_FALSE(rig.statuses().empty());
    CHECK(rig.statuses().back().find("API key") != std::string::npos);
}

// ---- the Remote engine (tests/test_remote_engine.py) ----

// OpenWhisper's notify_main_ui_ready starts the first load of any LocalSpeechBackend, and the
// Remote engine is one: its connection is made at startup, not at the first recording.
TEST_CASE("the remote engine connects when the main window is shown",
          "[test_remote_engine][widget]") {
    RigOptions options;
    options.settings["selected_model"] = "remote";
    ControllerRig rig(options);

    REQUIRE(rig.startEngine());

    CHECK(loads(rig.engine()) == 1);
    CHECK(rig.speech().readiness(fv::speech::Slot::Dictation).state ==
          fv::speech::ReadyState::Ready);
}

// from tests/test_remote_engine.py::test_local_engine_settings_leave_a_remote_engine_alone
TEST_CASE("test_local_engine_settings_leave_a_remote_engine_alone",
          "[test_remote_engine][widget]") {
    RigOptions options;
    options.settings["selected_model"] = "remote";
    ControllerRig rig(options);
    REQUIRE(rig.startEngine());

    // A local engine's model, device or quantization changed in Settings: they apply to this
    // computer's engines, which Remote isn't using.
    rig.app().on_local_engine_settings_changed();

    QApplication::processEvents();
    CHECK(rig.app().reload_machine().settled());
    CHECK(loads(rig.engine()) == 1);

    // With a local engine selected the same change reloads it.
    rig.settings().save_setting("selected_model", "parakeet");
    rig.app().on_local_engine_settings_changed();
    CHECK_FALSE(rig.app().reload_machine().settled());
    CHECK(waitUntil(
        [&rig] { return loads(rig.engine()) == 2 && rig.app().reload_machine().settled(); }));
}
