// Controller requests made off the GUI thread (tests/test_controller_thread_hops.py): hotkeys
// call the controller on threads of their own, and a timer only runs on the thread that owns
// it. These use the real executors and signals.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "controller_support.h"

using fvtest::ControllerRig;
using fvtest::waitUntil;

namespace {

template <class Fn> auto offQtThread(Fn fn) {
    std::optional<decltype(fn())> result;
    std::thread worker([&] { result.emplace(fn()); });
    worker.join();
    REQUIRE(result.has_value());
    return *result;
}

std::size_t loads(fv::speech::FakeEngine& engine) {
    const auto ops = engine.ops();
    return static_cast<std::size_t>(std::ranges::count(ops, std::string("load")));
}

} // namespace

// from
// tests/test_controller_thread_hops.py::test_a_hotkey_readiness_check_really_reloads_the_engine
// (never-loaded: the engine was not loaded yet; a Cancel does not close a SpeechService engine)
TEST_CASE("test_a_hotkey_readiness_check_really_reloads_the_engine",
          "[test_controller_thread_hops][widget]") {
    ControllerRig rig;

    const auto message =
        offQtThread([&rig] { return rig.app().transcription_readiness_message(); });

    CHECK(message == std::optional<std::string>("Reloading speech engine..."));
    // Other threads see the reload coming before the GUI thread arms it.
    CHECK_FALSE(rig.app().reload_machine().settled());
    CHECK(loads(rig.engine()) == 0);
    // The debounce runs on the GUI thread's event loop, then the engine loads.
    CHECK(waitUntil([&rig] { return loads(rig.engine()) == 1; }));
    CHECK(waitUntil([&rig] { return rig.app().reload_machine().settled(); }));
}

// from
// tests/test_controller_thread_hops.py::test_a_reload_asked_for_on_the_qt_thread_is_armed_at_once
TEST_CASE("test_a_reload_asked_for_on_the_qt_thread_is_armed_at_once",
          "[test_controller_thread_hops][widget]") {
    ControllerRig rig;

    rig.app().reload_whisper_model();

    CHECK_FALSE(rig.app().reload_machine().settled());
    CHECK_FALSE(rig.app().reload_machine().in_flight());
    CHECK(waitUntil(
        [&rig] { return rig.app().reload_machine().in_flight() || loads(rig.engine()) == 1; }));
}

// from
// tests/test_controller_thread_hops.py::test_recording_storage_failure_is_delivered_on_qt_thread
// (has_audio False, saved False: nothing was recorded)
TEST_CASE("test_recording_storage_failure_is_delivered_on_qt_thread",
          "[test_controller_thread_hops][widget]") {
    ControllerRig rig;
    std::vector<std::thread::id> delivered;
    auto link =
        rig.runtime().transcription_failed.connect(rig.uiExecutor(), [&delivered](std::string) {
            delivered.push_back(std::this_thread::get_id());
        });
    REQUIRE(rig.runtime().claim_job());

    offQtThread([&rig] {
        rig.runtime().finish_recording_job();
        return true;
    });
    CHECK(delivered.empty());
    CHECK(rig.runtime().has_active_job());

    REQUIRE(waitUntil([&] { return delivered.size() == 1 && !rig.runtime().has_active_job(); }));
    CHECK(delivered[0] == std::this_thread::get_id());
}

// from tests/test_controller_thread_hops.py::test_a_busy_hotkey_notice_is_shown_on_the_qt_thread
// (the shortcut's start reaches the runtime through the hotkey runtime's hop to the UI executor)
TEST_CASE("test_a_busy_hotkey_notice_is_shown_on_the_qt_thread",
          "[test_controller_thread_hops][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    std::vector<std::pair<std::string, std::thread::id>> shown;
    auto link =
        rig.runtime().notice_requested.connect(rig.uiExecutor(), [&shown](std::string text, bool) {
            shown.emplace_back(text, std::this_thread::get_id());
        });
    REQUIRE(rig.runtime().claim_job());

    // The hook's thread hands the press on.
    std::thread([&rig] { rig.pressKeypad("*"); }).join();
    CHECK(shown.empty());

    REQUIRE(waitUntil([&shown] { return shown.size() == 1; }));
    CHECK(shown[0].second == std::this_thread::get_id());
    CHECK(shown[0].first.find("already in progress") != std::string::npos);
    CHECK_FALSE(rig.runtime().is_recording());
    rig.runtime().finish_job();
}

// from
// tests/test_controller_thread_hops.py::test_feature_runtimes_deliver_worker_signals_on_the_qt_thread
// (the hotkey runtime's: transform_requested and hands_free_changed)
TEST_CASE("test_feature_runtimes_deliver_worker_signals_on_the_qt_thread",
          "[test_controller_thread_hops][widget]") {
    ControllerRig rig;
    std::vector<std::thread::id> transforms;
    std::vector<std::thread::id> handsFree;
    auto transformLink =
        rig.hotkeys().transform_requested.connect(rig.uiExecutor(), [&transforms](std::string) {
            transforms.push_back(std::this_thread::get_id());
        });
    auto handsLink = rig.hotkeys().hands_free_changed.connect(
        rig.uiExecutor(), [&handsFree](bool) { handsFree.push_back(std::this_thread::get_id()); });

    std::thread([&rig] {
        rig.hotkeys().transform_requested.emit("polish");
        rig.hotkeys().hands_free_changed.emit(true);
    }).join();
    CHECK(transforms.empty());
    CHECK(handsFree.empty());

    REQUIRE(waitUntil([&] { return transforms.size() == 1 && handsFree.size() == 1; }));
    CHECK(transforms[0] == std::this_thread::get_id());
    CHECK(handsFree[0] == std::this_thread::get_id());
    // The controller's own connection reached the overlay there too.
    CHECK(rig.overlay().handsFree());
}
