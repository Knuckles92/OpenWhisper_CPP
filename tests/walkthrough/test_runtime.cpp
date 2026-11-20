// The runtime ports are declared here (M6.6) and implemented in later waves: M6.10 (Snap, live
// drawing), M6.11 (recording) and M6.16 (panel, overlay, clicks, mark shortcuts). This test is
// the header's first translation unit: it includes it on its own and implements every port, so
// a port that does not compile, or a pure virtual that can't be overridden, shows up now.
#include "walkthrough/runtime_ports.h"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/env.h"
#include "base/identity.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "runtime_rig.h"
#include "settings/store.h"
#include "support.h"
#include "walkthrough/runtime.h"
#include "walkthrough/storage.h"
#include "walkthrough/testing/fake_shot_image.h"

using namespace fv::walkthrough;
using namespace fv::walkthrough::test;

TEST_CASE("walkthrough shortcuts manage screenshot modes and countdown", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->toggle();
    CHECK(rig.overlay.excluded);
    CHECK(rig.panel.snap_mode == "region");
    rig.runtime->choose_snap_mode("window");
    REQUIRE(rig.snap.requests.size() == 1);
    CHECK(rig.snap.requests.back().take == SnapTake::Window);
    CHECK(rig.snap.requests.back().front);
    rig.keep();
    rig.runtime->snap();
    REQUIRE(rig.snap.requests.size() == 2);
    CHECK(rig.snap.requests.back().take == SnapTake::Window);
    CHECK_FALSE(rig.snap.requests.back().front);
    rig.keep();
    rig.runtime->choose_snap_mode("delayed");
    CHECK(rig.panel.countdown == 3);
    CHECK(rig.overlay.caption == "Snapping in 3…");
    rig.ui.advance(std::chrono::seconds(1));
    CHECK(rig.panel.countdown == 2);
    rig.ui.advance(std::chrono::seconds(2));
    CHECK(rig.panel.countdown == 0);
    REQUIRE(rig.snap.requests.size() == 3);
    CHECK(rig.snap.requests.back().take == SnapTake::Region);
}

TEST_CASE("tray waits before screenshot and a menu choice replaces a countdown", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->toggle();
    rig.runtime->snap("tray");
    rig.ui.advance(std::chrono::milliseconds(299));
    CHECK(rig.snap.requests.empty());
    rig.ui.advance(std::chrono::milliseconds(1));
    REQUIRE(rig.snap.requests.size() == 1);
    CHECK(rig.snap.requests.back().front);
    rig.keep();
    rig.runtime->choose_snap_mode("delayed");
    CHECK(rig.panel.countdown == 3);
    rig.runtime->choose_snap_mode("screen");
    CHECK(rig.panel.countdown == 0);
    REQUIRE(rig.snap.requests.size() == 2);
    CHECK(rig.snap.requests.back().take == SnapTake::Screen);
    rig.ui.advance(std::chrono::seconds(4));
    CHECK(rig.snap.requests.size() == 2);
}

TEST_CASE("a plain screenshot copies its image, file and path", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->snap();
    REQUIRE(rig.snap.requests.size() == 1);
    CHECK(rig.snap.requests.front().take == SnapTake::Region);
    CHECK(rig.snap.requests.front().hint.empty());
    rig.keep();
    REQUIRE(rig.copied_image);
    REQUIRE(rig.copied.size() == 1);
    CHECK(std::filesystem::is_regular_file(rig.copied.front()));
    CHECK(rig.copied_text == fv::path_to_utf8(rig.copied.front()));
    CHECK(rig.notice == "Screenshot copied");
}

TEST_CASE("unsupported and failed screenshots report their reasons", "[runtime]") {
    RuntimeRig rig;
    rig.snap.supported = false;
    rig.snap.reason = "Screen capture unavailable";
    rig.runtime->snap();
    CHECK(rig.notice == "Screen capture unavailable");
    CHECK(rig.snap.access_requests == 1);
    CHECK(rig.snap.requests.empty());
    rig.snap.supported = true;
    rig.runtime->snap();
    REQUIRE(rig.snap.failed_);
    rig.snap.failed_("Capture failed");
    rig.ui.run_all();
    CHECK(rig.notice == "Capture failed");
}

TEST_CASE("walkthrough board moves and removes screenshots and keeps their labels", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->toggle();
    rig.runtime->snap();
    rig.keep();
    CHECK(rig.recording.label == "Walkthrough · 1 shot");
    rig.runtime->snap();
    rig.keep();
    CHECK(rig.recording.label == "Walkthrough · 2 shots");
    CHECK(rig.panel.added == std::vector<int>{1, 2});
    rig.runtime->move_shot(2, -1);
    CHECK(rig.panel.moved == std::vector<std::pair<int, int>>{{2, -1}});
    rig.runtime->remove_shot(1);
    CHECK(rig.panel.removed == std::vector<int>{1});
    CHECK(rig.recording.label == "Walkthrough · 1 shot");
    rig.runtime->undo_last_shot();
    CHECK(rig.panel.removed == std::vector<int>{1, 2});
    CHECK(rig.recording.label == "Walkthrough");
}

TEST_CASE("column cancel, note and pause controls reach the recording", "[runtime]") {
    RuntimeRig rig;
    rig.settings.save_setting("walkthrough_write_up", true);
    rig.runtime->toggle();
    CHECK(rig.panel.notes_on);
    rig.recording.can_pause = false;
    rig.runtime->set_paused(true);
    CHECK_FALSE(rig.panel.paused());
    rig.recording.can_pause = true;
    rig.runtime->set_paused(true);
    CHECK(rig.panel.paused());
    CHECK(rig.recording.status == "Walkthrough · Paused");
    rig.runtime->set_paused(false);
    CHECK_FALSE(rig.panel.paused());
    CHECK(rig.recording.status == "Walkthrough · Recording...");
    rig.recording.duration = 2;
    rig.runtime->hold_note(true);
    rig.recording.duration = 4;
    rig.runtime->hold_note(false);
    const auto board = rig.recording.recording_walkthrough()->storyboard();
    REQUIRE(board.notes_s.size() == 1);
    CHECK(board.notes_s[0] == std::pair{2.0, 4.0});
    rig.panel.cancel_requested.emit();
    rig.ui.run_all();
    CHECK(rig.recording.cancels == 1);
    rig.recording.writing = true;
    rig.runtime->cancel();
    CHECK_FALSE(rig.recording.writing);
    CHECK(rig.recording.cancels == 1);
}

TEST_CASE("canceling or stopping a walkthrough resolves an open screenshot", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->toggle();
    rig.runtime->snap();
    auto handle = rig.snap.handle;
    rig.recording.canceling = true;
    rig.runtime->on_recording_state_changed(false);
    CHECK(handle->drops == 1);
    CHECK_FALSE(rig.overlay.excluded);
    rig.snap.dropped_();
    rig.ui.run_all();
    CHECK(rig.recording.recording_walkthrough()->shot_count() == 0);
}

TEST_CASE("finishing waits for a dropped screenshot", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->toggle();
    rig.runtime->snap();
    rig.runtime->finish();
    CHECK(rig.recording.stops == 0);
    rig.snap.dropped_();
    rig.ui.run_all();
    CHECK(rig.recording.stops == 1);
}

TEST_CASE("Done waits for a stroke and captures its pending drawing", "[runtime]") {
    RuntimeRig rig(true);
    rig.settings.save_setting("walkthrough_capture_delay", 2);
    rig.runtime->toggle();
    rig.drawing->drawn.emit(ScreenRef{"display", 0}, std::make_shared<int>(1));
    rig.ui.run_all();
    rig.drawing->interacting_ = true;
    rig.drawing->interaction_changed.emit(true);
    rig.ui.run_all();
    rig.runtime->finish();
    rig.ui.advance(std::chrono::seconds(3));
    CHECK(rig.snap.requests.empty());
    CHECK(rig.recording.stops == 0);
    rig.drawing->interacting_ = false;
    rig.drawing->interaction_changed.emit(false);
    rig.ui.run_all();
    REQUIRE(rig.snap.requests.size() == 1);
    rig.keep();
    CHECK(rig.recording.stops == 1);
}

TEST_CASE("auto capture cancels delayed clicks and ignores pre-pause events", "[runtime]") {
    RuntimeRig rig(true);
    rig.settings.save_setting("walkthrough_capture_delay", 1);
    rig.runtime->toggle();
    REQUIRE(rig.click);
    rig.click(10, 10, 0);
    rig.ui.run_all();
    rig.ui.advance(std::chrono::milliseconds(100));
    rig.runtime->set_paused(true);
    rig.runtime->set_paused(false);
    rig.click(11, 11, 0);
    rig.ui.run_all();
    rig.ui.advance(std::chrono::seconds(2));
    CHECK(rig.snap.requests.empty());
    rig.click(12, 12, 2.1);
    rig.ui.run_all();
    rig.ui.advance(std::chrono::seconds(1));
    REQUIRE(rig.snap.requests.size() == 1);
    CHECK(rig.snap.requests[0].click_at == std::pair{12, 12});
}

TEST_CASE("auto mark shortcuts follow enabled drawing and are removed on cleanup", "[runtime]") {
    RuntimeRig rig(true);
    rig.runtime->toggle();
    CHECK(rig.registered.size() == 2);
    REQUIRE(rig.mark);
    rig.mark("keep_marks");
    rig.ui.run_all();
    CHECK(rig.drawing->marked == std::vector<std::string>{"keep_marks"});
    rig.drawing->enabled_changed.emit(false);
    rig.ui.run_all();
    CHECK(rig.registered.empty());
    rig.drawing->enabled_changed.emit(true);
    rig.ui.run_all();
    CHECK(rig.registered.size() == 2);
    rig.runtime->cleanup();
    CHECK(rig.registered.empty());
    CHECK(rig.drawing->closed);
}

TEST_CASE("auto mark shortcuts leave existing bindings alone", "[runtime]") {
    RuntimeRig rig(true);
    rig.conflict_key = "f8";
    rig.runtime->toggle();
    CHECK_FALSE(rig.registered.contains("keep_marks"));
    CHECK(rig.registered.at("clear") == "f9");
}

TEST_CASE("auto drawing crops the selected region and retains marks", "[runtime]") {
    RuntimeRig rig(true);
    rig.runtime->toggle();
    const ScreenRef screen{"display", 0};
    rig.drawing->region = CaptureRegion{screen, Rect{10, 20, 80, 60}, Rect{110, 220, 80, 60}};
    auto marks = std::make_shared<int>(4);
    rig.drawing->drawn.emit(screen, marks);
    rig.ui.run_all();
    REQUIRE(rig.snap.requests.size() == 1);
    rig.keep();
    CHECK(rig.snap.render_crop == Rect{10, 20, 80, 60});
    CHECK(rig.snap.render_marks == marks);
    CHECK(rig.recording.recording_walkthrough()->shot_count() == 1);
}

TEST_CASE("auto clicks use the selected region and its pinned marks", "[runtime]") {
    for (double ratio : {1.0, 2.0}) {
        RuntimeRig rig(true);
        rig.runtime->toggle();
        const ScreenRef screen{"display", 0};
        rig.drawing->region = CaptureRegion{screen, Rect{10, 20, 80, 60}, Rect{110, 220, 80, 60}};
        auto marks = std::make_shared<int>(4);
        rig.drawing->pinned = marks;
        rig.click(150, 250, 0);
        rig.ui.run_all();
        rig.ui.advance(std::chrono::milliseconds(350));
        REQUIRE(rig.snap.requests.size() == 1);
        CHECK(rig.snap.requests[0].screen == screen);
        rig.snap.kept_(
            std::make_shared<FakeShotImage>(2, 2, 0x123456, FakeShotImage::Save::Works, ratio));
        rig.ui.run_all();
        CHECK(rig.snap.render_crop == Rect{10, 20, 80, 60});
        CHECK(rig.snap.render_marks == marks);
    }
}

TEST_CASE("closing an auto capture region drops pending and old queued clicks", "[runtime]") {
    RuntimeRig rig(true);
    rig.settings.save_setting("walkthrough_capture_delay", 1);
    rig.runtime->toggle();
    rig.click(1, 2, 0);
    rig.ui.run_all();
    rig.ui.advance(std::chrono::milliseconds(100));
    rig.drawing->capture_changed.emit();
    rig.ui.run_all();
    rig.click(2, 3, 0);
    rig.ui.run_all();
    rig.ui.advance(std::chrono::seconds(2));
    CHECK(rig.snap.requests.empty());
}

TEST_CASE("auto drawing takes priority over a pending click", "[runtime]") {
    RuntimeRig rig(true);
    rig.settings.save_setting("walkthrough_capture_delay", 1);
    rig.runtime->toggle();
    rig.click(1, 2, 0);
    rig.ui.run_all();
    const ScreenRef screen{"display", 0};
    rig.drawing->drawn.emit(screen, std::make_shared<int>(1));
    rig.ui.run_all();
    rig.ui.advance(std::chrono::seconds(1));
    REQUIRE(rig.snap.requests.size() == 1);
    CHECK(rig.snap.requests.front().screen == screen);
    CHECK_FALSE(rig.snap.requests.front().click_at);
    rig.keep();
    CHECK(rig.recording.recording_walkthrough()->shot_count() == 1);
}

TEST_CASE("a manual screenshot left open after recording is copied", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->toggle();
    rig.runtime->snap();
    auto owner = rig.recording.recording_walkthrough();
    owner->close();
    rig.runtime->on_recording_state_changed(false);
    CHECK(rig.snap.handle->keeps == 1);
    rig.keep();
    CHECK(owner->shot_count() == 0);
    REQUIRE(rig.copied_image);
    CHECK(rig.notice == "Screenshot copied — the walkthrough had already stopped");
}

TEST_CASE("ending a different recording leaves the walkthrough overlay alone", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->on_recording_state_changed(false);
    CHECK_FALSE(rig.overlay.excluded);
    CHECK(rig.snap.requests.empty());
    CHECK(rig.recording.stops == 0);
}

TEST_CASE("walkthrough sessions start at the current clock time", "[runtime]") {
    RuntimeRig rig;
    rig.clock.advance(std::chrono::seconds(42));
    rig.runtime->toggle();
    const auto expected =
        std::chrono::local_days{std::chrono::year{2026} / 1 / 1} + std::chrono::seconds(42);
    CHECK(rig.recording.recording_walkthrough()->started_at() == expected);
}

TEST_CASE("walkthrough shortcut starts with the overlay kept out of screenshots", "[runtime]") {
    RuntimeRig rig;
    std::vector<bool> active;
    auto link = rig.runtime->walkthrough_active.connect(
        rig.ui, [&active](bool value) { active.push_back(value); });
    rig.runtime->toggle();
    rig.ui.run_all();
    CHECK(active == std::vector<bool>{true});
    CHECK(rig.overlay.excluded);
    CHECK(rig.recording.recording_walkthrough());
}

TEST_CASE("notes are offered only when write-up is enabled", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->toggle();
    CHECK_FALSE(rig.panel.notes_on);
    RuntimeRig other;
    other.settings.save_setting("walkthrough_write_up", true);
    other.runtime->toggle();
    CHECK(other.panel.notes_on);
}

TEST_CASE("stopping a walkthrough cancels its countdown", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->toggle();
    rig.runtime->choose_snap_mode("delayed");
    REQUIRE(rig.panel.countdown == 3);
    rig.runtime->on_recording_state_changed(false);
    CHECK(rig.panel.countdown == 0);
    rig.ui.advance(std::chrono::seconds(4));
    CHECK(rig.snap.requests.empty());
}

TEST_CASE("Done shortens a delayed click to its settle time", "[runtime]") {
    RuntimeRig rig(true);
    rig.settings.save_setting("walkthrough_capture_delay", 5);
    rig.runtime->toggle();
    rig.click(1, 2, 0);
    rig.ui.run_all();
    rig.runtime->finish();
    rig.ui.advance(std::chrono::milliseconds(349));
    CHECK(rig.snap.requests.empty());
    rig.ui.advance(std::chrono::milliseconds(1));
    REQUIRE(rig.snap.requests.size() == 1);
    CHECK(rig.recording.stops == 0);
    rig.keep();
    CHECK(rig.recording.stops == 1);
}

TEST_CASE("closing an auto region while finishing releases the pending recording", "[runtime]") {
    RuntimeRig rig(true);
    rig.settings.save_setting("walkthrough_capture_delay", 5);
    rig.runtime->toggle();
    rig.click(1, 2, 0);
    rig.ui.run_all();
    rig.runtime->finish();
    CHECK(rig.recording.stops == 0);
    rig.drawing->capture_changed.emit();
    rig.ui.run_all();
    CHECK(rig.recording.stops == 1);
    rig.ui.advance(std::chrono::seconds(6));
    CHECK(rig.snap.requests.empty());
}

TEST_CASE("a pending delayed drawing can be canceled", "[runtime]") {
    RuntimeRig rig(true);
    rig.settings.save_setting("walkthrough_capture_delay", 1);
    rig.runtime->toggle();
    rig.drawing->drawn.emit(ScreenRef{"display", 0}, std::make_shared<int>(1));
    rig.ui.run_all();
    REQUIRE(rig.drawing->pending);
    rig.drawing->capture_cancelled.emit();
    rig.ui.run_all();
    CHECK_FALSE(rig.drawing->pending);
    rig.ui.advance(std::chrono::seconds(2));
    CHECK(rig.snap.requests.empty());

    RuntimeRig changed(true);
    changed.settings.save_setting("walkthrough_capture_delay", 1);
    changed.runtime->toggle();
    changed.drawing->drawn.emit(ScreenRef{"display", 0}, std::make_shared<int>(1));
    changed.ui.run_all();
    changed.drawing->capture_changed.emit();
    changed.ui.run_all();
    changed.ui.advance(std::chrono::seconds(2));
    CHECK(changed.snap.requests.empty());
}

TEST_CASE("pausing drops an in-flight automatic screenshot", "[runtime]") {
    RuntimeRig rig(true);
    rig.runtime->toggle();
    rig.click(1, 2, 0);
    rig.ui.run_all();
    rig.ui.advance(std::chrono::milliseconds(350));
    REQUIRE(rig.snap.handle);
    auto handle = rig.snap.handle;
    auto old_callback = rig.snap.kept_;
    rig.runtime->set_paused(true);
    CHECK(handle->drops == 1);
    old_callback(std::make_shared<FakeShotImage>(2, 2, 0x123456));
    rig.ui.run_all();
    CHECK(rig.recording.recording_walkthrough()->shot_count() == 0);

    RuntimeRig ending(true);
    ending.runtime->toggle();
    ending.click(1, 2, 0);
    ending.ui.run_all();
    ending.ui.advance(std::chrono::milliseconds(350));
    REQUIRE(ending.snap.handle);
    auto ending_handle = ending.snap.handle;
    auto ending_callback = ending.snap.kept_;
    ending.runtime->on_recording_state_changed(false);
    CHECK(ending_handle->drops == 1);
    ending_callback(std::make_shared<FakeShotImage>(2, 2, 0x123456));
    ending.ui.run_all();
    CHECK(ending.recording.recording_walkthrough()->shot_count() == 0);
}

TEST_CASE("a screenshot that cannot be saved to disk is still copied", "[runtime]") {
    RuntimeRig rig;
    const auto captures = fv::walkthrough::captures_root(rig.paths);
    std::filesystem::create_directories(captures);
    fv::walkthrough::test::write_file(captures / "screenshots", "not a directory");
    rig.runtime->snap();
    rig.keep();
    REQUIRE(rig.copied_image);
    CHECK(rig.copied.empty());
    CHECK(rig.copied_text.empty());
    CHECK(rig.notice == "Screenshot copied (its file couldn't be saved)");
}

TEST_CASE("the column's buttons are connected only once", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->attach_panel(&rig.panel);
    rig.runtime->attach_panel(&rig.panel);
    rig.runtime->toggle();
    rig.panel.cancel_requested.emit();
    rig.ui.run_all();
    CHECK(rig.recording.cancels == 1);
}

TEST_CASE("the column's Snap takes the front window", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->toggle();
    rig.runtime->choose_snap_mode("window");
    rig.keep();
    rig.panel.snap_requested.emit();
    rig.ui.run_all();
    REQUIRE(rig.snap.requests.size() == 2);
    CHECK(rig.snap.requests.back().take == SnapTake::Window);
    CHECK(rig.snap.requests.back().front);
}

TEST_CASE("the column's Snap and Done keep a screenshot and finish", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->toggle();
    rig.panel.snap_requested.emit();
    rig.ui.run_all();
    REQUIRE(rig.snap.handle);
    rig.panel.finish_requested.emit();
    rig.ui.run_all();
    CHECK(rig.snap.handle->keeps == 1);
    CHECK(rig.recording.stops == 0);
    rig.keep();
    CHECK(rig.recording.recording_walkthrough()->shot_count() == 1);
    CHECK(rig.recording.stops == 1);
}

TEST_CASE("delayed drawings on one screen restart and combine", "[runtime]") {
    RuntimeRig rig(true);
    rig.settings.save_setting("walkthrough_capture_delay", 1);
    rig.runtime->toggle();
    const ScreenRef screen{"display", 0};
    auto first = std::make_shared<int>(1);
    auto second = std::make_shared<int>(2);
    rig.drawing->drawn.emit(screen, first);
    rig.ui.run_all();
    rig.ui.advance(std::chrono::milliseconds(500));
    rig.drawing->drawn.emit(screen, second);
    rig.ui.run_all();
    rig.ui.advance(std::chrono::milliseconds(500));
    CHECK(rig.snap.requests.empty());
    rig.ui.advance(std::chrono::milliseconds(500));
    REQUIRE(rig.snap.requests.size() == 1);
    rig.keep();
    CHECK(rig.snap.render_marks == second);
    CHECK(rig.recording.recording_walkthrough()->shot_count() == 1);
}

TEST_CASE("a failed delayed drawing leaves its marks pending", "[runtime]") {
    RuntimeRig rig(true);
    rig.settings.save_setting("walkthrough_capture_delay", 1);
    rig.runtime->toggle();
    rig.drawing->drawn.emit(ScreenRef{"display", 0}, std::make_shared<int>(1));
    rig.ui.run_all();
    rig.ui.advance(std::chrono::seconds(1));
    REQUIRE(rig.snap.failed_);
    rig.snap.failed_("Capture failed");
    rig.ui.run_all();
    CHECK_FALSE(rig.drawing->last_captured);
    CHECK_FALSE(rig.drawing->pending);
    CHECK_FALSE(rig.drawing->busy);
    CHECK(rig.recording.recording_walkthrough()->shot_count() == 0);
    CHECK(rig.notice == "Capture failed");
}

TEST_CASE("a delayed drawing that cannot be saved retains its marks", "[runtime]") {
    RuntimeRig rig(true);
    const auto captures = fv::walkthrough::captures_root(rig.paths);
    std::filesystem::create_directories(captures.parent_path());
    fv::walkthrough::test::write_file(captures, "not a directory");
    rig.settings.save_setting("walkthrough_capture_delay", 1);
    rig.runtime->toggle();
    rig.drawing->drawn.emit(ScreenRef{"display", 0}, std::make_shared<int>(1));
    rig.ui.run_all();
    rig.ui.advance(std::chrono::seconds(1));
    REQUIRE(rig.snap.kept_);
    rig.keep();
    CHECK_FALSE(rig.drawing->pending);
    CHECK_FALSE(rig.drawing->busy);
    CHECK_FALSE(rig.drawing->last_captured);
    CHECK(rig.recording.recording_walkthrough()->shot_count() == 0);
    CHECK(rig.notice.find("Couldn't save the screenshot") != std::string::npos);
}

TEST_CASE("capture delay changes reschedule a pending click", "[runtime]") {
    RuntimeRig rig(true);
    rig.settings.save_setting("walkthrough_capture_delay", 1);
    rig.runtime->toggle();
    rig.click(1, 2, 0);
    rig.ui.run_all();
    rig.settings.save_setting("walkthrough_capture_delay", 3);
    rig.drawing->capture_delay_changed.emit(3);
    rig.ui.run_all();
    rig.ui.advance(std::chrono::seconds(2));
    CHECK(rig.snap.requests.empty());
    rig.ui.advance(std::chrono::seconds(1));
    CHECK(rig.snap.requests.size() == 1);

    RuntimeRig other(true);
    other.settings.save_setting("walkthrough_capture_delay", 1);
    other.runtime->toggle();
    other.drawing->drawn.emit(ScreenRef{"display", 0}, std::make_shared<int>(1));
    other.ui.run_all();
    other.settings.save_setting("walkthrough_capture_delay", 3);
    other.drawing->capture_delay_changed.emit(3);
    other.ui.run_all();
    other.ui.advance(std::chrono::seconds(2));
    CHECK(other.snap.requests.empty());
    other.ui.advance(std::chrono::seconds(1));
    CHECK(other.snap.requests.size() == 1);
}

TEST_CASE("a walkthrough screenshot whose folder cannot be made reports the failure", "[runtime]") {
    RuntimeRig rig;
    fv::walkthrough::test::write_file(fv::walkthrough::captures_root(rig.paths), "not a folder");
    rig.runtime->toggle();
    rig.runtime->snap();
    rig.keep();
    CHECK(rig.recording.recording_walkthrough()->shot_count() == 0);
    CHECK(rig.notice.find("Couldn't save the screenshot (") == 0);
    CHECK(rig.notice.find("the walkthrough keeps recording") != std::string::npos);
    CHECK_FALSE(rig.copied_image);
}

TEST_CASE("walkthrough starts and chooses a screenshot mode", "[runtime]") {
    fv::walkthrough::test::TempFolder folder;
    fv::MapEnvReader env{{"FIORIVOCE_DATA_DIR", fv::path_to_utf8(folder.path())}};
    const auto paths = fv::Paths::resolve(fv::identity(), env);
    fv::SettingsStore settings(folder / "settings.json");
    fv::FakeClock clock;
    fv::ManualExecutor ui(clock);
    Recording recording;
    Snap snap;
    Overlay overlay;
    WalkthroughRuntime runtime({ui, clock, settings, paths, recording, snap, overlay});
    runtime.toggle("ui");
    REQUIRE(recording.recording_walkthrough());
    CHECK(recording.source_ == "ui");
    runtime.choose_snap_mode("window");
    REQUIRE(snap.requests.size() == 1);
    CHECK(snap.requests[0].take == SnapTake::Window);
    CHECK(snap.requests[0].front);
    CHECK(snap.requests[0].hint == "Recording continues — talk while you draw");
    CHECK(shots_label(0) == "Walkthrough");
    CHECK(shots_label(1) == "Walkthrough · 1 shot");
    CHECK(shots_label(2) == "Walkthrough · 2 shots");
}

TEST_CASE("delayed snap can be taken early", "[runtime]") {
    fv::walkthrough::test::TempFolder folder;
    fv::MapEnvReader env{{"FIORIVOCE_DATA_DIR", fv::path_to_utf8(folder.path())}};
    const auto paths = fv::Paths::resolve(fv::identity(), env);
    fv::SettingsStore settings(folder / "settings.json");
    fv::FakeClock clock;
    fv::ManualExecutor ui(clock);
    Recording recording;
    Snap snap;
    Overlay overlay;
    WalkthroughRuntime runtime({ui, clock, settings, paths, recording, snap, overlay});
    runtime.toggle();
    runtime.choose_snap_mode("delayed");
    CHECK(snap.requests.empty());
    runtime.snap();
    REQUIRE(snap.requests.size() == 1);
    CHECK(snap.requests[0].take == SnapTake::Region);
    ui.advance(std::chrono::seconds(4));
    CHECK(snap.requests.size() == 1);
}

TEST_CASE("auto clicks coalesce and capture after settling", "[runtime]") {
    fv::walkthrough::test::TempFolder folder;
    fv::MapEnvReader env{{"FIORIVOCE_DATA_DIR", fv::path_to_utf8(folder.path())}};
    const auto paths = fv::Paths::resolve(fv::identity(), env);
    fv::SettingsStore settings(folder / "settings.json");
    settings.save_setting("walkthrough_auto_mode", true);
    fv::FakeClock clock;
    fv::ManualExecutor ui(clock);
    Recording recording;
    Snap snap;
    Overlay overlay;
    auto drawing = std::make_shared<LiveDrawing>();
    std::function<void(int, int, double)> clicked;
    RuntimeServices services{ui, clock, settings, paths, recording, snap, overlay};
    services.platform = "win32";
    services.live = [drawing](auto) {
        return drawing;
    };
    services.clicks = [&clicked](auto callback) {
        clicked = std::move(callback);
        return std::make_unique<fv::Subscription>();
    };
    WalkthroughRuntime runtime(std::move(services));
    runtime.toggle();
    REQUIRE(clicked);
    clicked(1, 2, 0);
    ui.run_all();
    ui.advance(std::chrono::milliseconds(100));
    clicked(3, 4, 0.1);
    ui.run_all();
    ui.advance(std::chrono::milliseconds(249));
    CHECK(snap.requests.empty());
    ui.advance(std::chrono::milliseconds(1));
    REQUIRE(snap.requests.size() == 1);
    CHECK(snap.requests[0].take == SnapTake::Screen);
    CHECK(snap.requests[0].click_at == std::pair{3, 4});
    REQUIRE(snap.kept_);
    snap.kept_(std::make_shared<FakeShotImage>(2, 2, 0x123456));
    ui.run_all();
    CHECK(recording.recording_walkthrough()->shot_count() == 1);
}

TEST_CASE("walkthrough start explains recording, write-up and readiness refusals", "[runtime]") {
    fv::walkthrough::test::TempFolder folder;
    fv::MapEnvReader env{{"FIORIVOCE_DATA_DIR", fv::path_to_utf8(folder.path())}};
    const auto paths = fv::Paths::resolve(fv::identity(), env);
    fv::SettingsStore settings(folder / "settings.json");
    fv::FakeClock clock;
    fv::ManualExecutor ui(clock);
    Recording recording;
    Snap snap;
    Overlay overlay;
    std::string notice;
    RuntimeServices services{ui, clock, settings, paths, recording, snap, overlay};
    services.tell = [&notice](std::string text, bool) {
        notice = std::move(text);
    };
    WalkthroughRuntime runtime(std::move(services));
    recording.other_recording = true;
    runtime.toggle();
    CHECK(notice == "Finish the recording in progress first");
    recording.other_recording = false;
    recording.writing = true;
    runtime.toggle();
    CHECK(notice == "Wait for the last walkthrough's write-up, or cancel it");
    recording.writing = false;
    recording.unavailable = "Engine is loading";
    runtime.toggle();
    CHECK(notice == "Engine is loading");
    CHECK_FALSE(recording.recording_walkthrough());
}

TEST_CASE("finishing a walkthrough keeps an open screenshot before stopping", "[runtime]") {
    fv::walkthrough::test::TempFolder folder;
    fv::MapEnvReader env{{"FIORIVOCE_DATA_DIR", fv::path_to_utf8(folder.path())}};
    const auto paths = fv::Paths::resolve(fv::identity(), env);
    fv::SettingsStore settings(folder / "settings.json");
    fv::FakeClock clock;
    fv::ManualExecutor ui(clock);
    Recording recording;
    Snap snap;
    Overlay overlay;
    WalkthroughRuntime runtime({ui, clock, settings, paths, recording, snap, overlay});
    runtime.toggle();
    runtime.snap();
    REQUIRE(snap.handle);
    runtime.finish();
    CHECK(snap.handle->keeps == 1);
    CHECK(recording.stops == 0);
    snap.kept_(std::make_shared<FakeShotImage>(2, 2, 0x123456));
    ui.run_all();
    CHECK(recording.recording_walkthrough()->shot_count() == 1);
    CHECK(recording.stops == 1);
}

TEST_CASE("pressing screenshot again keeps the open selection", "[runtime]") {
    fv::walkthrough::test::TempFolder folder;
    fv::MapEnvReader env{{"FIORIVOCE_DATA_DIR", fv::path_to_utf8(folder.path())}};
    const auto paths = fv::Paths::resolve(fv::identity(), env);
    fv::SettingsStore settings(folder / "settings.json");
    fv::FakeClock clock;
    fv::ManualExecutor ui(clock);
    Recording recording;
    Snap snap;
    Overlay overlay;
    WalkthroughRuntime runtime({ui, clock, settings, paths, recording, snap, overlay});
    runtime.toggle();
    runtime.snap();
    runtime.snap();
    REQUIRE(snap.handle);
    CHECK(snap.handle->keeps == 1);
    CHECK(snap.requests.size() == 1);
}

TEST_CASE("Done captures a pending drawing before stopping", "[runtime]") {
    fv::walkthrough::test::TempFolder folder;
    fv::MapEnvReader env{{"FIORIVOCE_DATA_DIR", fv::path_to_utf8(folder.path())}};
    const auto paths = fv::Paths::resolve(fv::identity(), env);
    fv::SettingsStore settings(folder / "settings.json");
    settings.save_setting("walkthrough_auto_mode", true);
    settings.save_setting("walkthrough_capture_delay", 1);
    fv::FakeClock clock;
    fv::ManualExecutor ui(clock);
    Recording recording;
    Snap snap;
    Overlay overlay;
    auto drawing = std::make_shared<LiveDrawing>();
    RuntimeServices services{ui, clock, settings, paths, recording, snap, overlay};
    services.platform = "win32";
    services.live = [drawing](auto) {
        return drawing;
    };
    services.clicks = [](auto) {
        return std::make_unique<fv::Subscription>();
    };
    WalkthroughRuntime runtime(std::move(services));
    runtime.toggle();
    const ScreenRef screen{"display 2", 1};
    drawing->drawn.emit(screen, std::make_shared<int>(1));
    ui.run_all();
    CHECK(snap.requests.empty());
    runtime.finish();
    REQUIRE(snap.requests.size() == 1);
    CHECK(snap.requests[0].screen == screen);
    CHECK(recording.stops == 0);
    snap.kept_(std::make_shared<FakeShotImage>(2, 2, 0x123456));
    ui.run_all();
    CHECK(recording.recording_walkthrough()->shot_count() == 1);
    CHECK(recording.stops == 1);
}

TEST_CASE("drawings on different screens get separate screenshots", "[runtime]") {
    fv::walkthrough::test::TempFolder folder;
    fv::MapEnvReader env{{"FIORIVOCE_DATA_DIR", fv::path_to_utf8(folder.path())}};
    const auto paths = fv::Paths::resolve(fv::identity(), env);
    fv::SettingsStore settings(folder / "settings.json");
    settings.save_setting("walkthrough_auto_mode", true);
    settings.save_setting("walkthrough_capture_delay", 1);
    fv::FakeClock clock;
    fv::ManualExecutor ui(clock);
    Recording recording;
    Snap snap;
    Overlay overlay;
    auto drawing = std::make_shared<LiveDrawing>();
    RuntimeServices services{ui, clock, settings, paths, recording, snap, overlay};
    services.platform = "win32";
    services.live = [drawing](auto) {
        return drawing;
    };
    services.clicks = [](auto) {
        return std::make_unique<fv::Subscription>();
    };
    WalkthroughRuntime runtime(std::move(services));
    runtime.toggle();
    const ScreenRef first{"first", 0}, second{"second", 1};
    drawing->drawn.emit(first, std::make_shared<int>(1));
    ui.run_all();
    drawing->drawn.emit(second, std::make_shared<int>(2));
    ui.run_all();
    REQUIRE(snap.requests.size() == 1);
    CHECK(snap.requests[0].screen == first);
    snap.kept_(std::make_shared<FakeShotImage>(2, 2, 0x123456));
    ui.run_all();
    ui.advance(std::chrono::seconds(1));
    REQUIRE(snap.requests.size() == 2);
    CHECK(snap.requests[1].screen == second);
    snap.kept_(std::make_shared<FakeShotImage>(2, 2, 0x654321));
    ui.run_all();
    CHECK(recording.recording_walkthrough()->shot_count() == 2);
}

TEST_CASE("late snap callbacks are ignored after runtime destruction", "[runtime]") {
    fv::walkthrough::test::TempFolder folder;
    fv::MapEnvReader env{{"FIORIVOCE_DATA_DIR", fv::path_to_utf8(folder.path())}};
    const auto paths = fv::Paths::resolve(fv::identity(), env);
    fv::SettingsStore settings(folder / "settings.json");
    fv::FakeClock clock;
    fv::ManualExecutor ui(clock);
    Recording recording;
    Snap snap;
    Overlay overlay;
    {
        WalkthroughRuntime runtime({ui, clock, settings, paths, recording, snap, overlay});
        runtime.toggle();
        runtime.snap();
        REQUIRE(snap.kept_);
    }
    CHECK(snap.handle->drops == 1);
    snap.kept_(std::make_shared<FakeShotImage>(2, 2, 0x123456));
    ui.run_all();
    CHECK(recording.recording_walkthrough()->shot_count() == 0);
}
