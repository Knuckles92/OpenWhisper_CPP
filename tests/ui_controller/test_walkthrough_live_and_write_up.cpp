// Auto Mode's live drawing and the AI write-up in the running app (ui_controller's
// start_walkthrough_drawing, _walkthrough_panel_stands_in and _dismiss_walkthrough_panel_for_job):
// the live toolbar's toggles are saved where Snap's are, and during the write-up the overlay
// shows a dictation and a failure leaves the column with its Cancel.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>

#include <QApplication>
#include <QTest>

#include "clipboard/testing/fake_backend.h"
#include "controller_support.h"
#include "dictation/transcription_runtime.h"
#include "dictation/walkthrough_job.h"
#include "hotkeys/testing/fake_mouse_hook.h"
#include "settings/keys.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_snap/testing/fake_desktop.h"
#include "ui_snap/toolbar.h"
#include "ui_walkthrough/walkthrough_panel.h"
#include "walkthrough/runtime_ports.h"
#include "walkthrough/testing/fake_shot_image.h"

namespace {

class FakeHandle final : public fv::walkthrough::SnapHandle {
public:
    bool active() const override { return open; }
    void keep() override { open = false; }
    void drop() override { open = false; }
    std::optional<fv::walkthrough::Rect> capture_rect() const override { return {}; }
    bool open = true;
};

class FakeSnap final : public fv::walkthrough::SnapPort {
public:
    std::pair<bool, std::string> support() const override { return {true, {}}; }
    void request_access() override {}
    std::shared_ptr<fv::walkthrough::SnapHandle> start(fv::walkthrough::SnapRequest, Kept callback,
                                                       Dropped, Failed) override {
        kept = std::move(callback);
        return std::make_shared<FakeHandle>();
    }
    fv::walkthrough::ShotImagePtr render(fv::walkthrough::ShotImagePtr image,
                                         std::optional<fv::walkthrough::Rect>,
                                         fv::walkthrough::MarksPtr) override {
        return image;
    }
    bool window_mode_supported() const override { return true; }
    Kept kept;
};

fvtest::RigOptions rigWith(FakeSnap& snap, fv::Json settings) {
    fvtest::RigOptions options;
    options.walkthrough = true;
    options.walkthroughSnap = &snap;
    options.settings = std::move(settings);
    options.walkthroughClicks = [](fv::hotkeys::ClickObserver observer) {
        return std::make_unique<fv::hotkeys::FakeMouseHook>(std::move(observer));
    };
    options.platform = [](fv::ControllerPlatform& platform) {
        platform.clipboardBackend = [] {
            return std::make_unique<fv::clipboard::FakeClipboardBackend>();
        };
    };
    return options;
}

template <typename T> T* topLevel() {
    for (QWidget* widget : QApplication::allWidgets()) {
        if (auto* found = qobject_cast<T*>(widget))
            return found;
    }
    return nullptr;
}

} // namespace

TEST_CASE("auto mode's live toolbar saves snap to edges and keep marks", "[widget]") {
#if !defined(_WIN32) && !defined(__APPLE__)
    // The runtime offers Auto Mode on win32 and darwin only (walkthrough.py's platform check).
    SKIP("Auto Mode is not offered on this platform");
#endif
    fv::ui_snap::testing::ScopedFakeDesktop desktop;
    FakeSnap snap;
    fvtest::ControllerRig rig(rigWith(snap, fv::Json{{"walkthrough_auto_mode", true}}));
    REQUIRE(rig.startEngine());
    const bool edges = rig.context().settings().get(fv::keys::snap_to_edges);
    REQUIRE_FALSE(rig.context().settings().get(fv::keys::walkthrough_keep_marks));

    Q_EMIT rig.window().walkthroughRequested();
    REQUIRE(fvtest::waitUntil([&rig] { return rig.runtime().is_recording(); }));
    fv::ui_snap::SnapToolbar* toolbar = nullptr;
    REQUIRE(fvtest::waitUntil(
        [&toolbar] { return (toolbar = topLevel<fv::ui_snap::SnapToolbar>()) != nullptr; }));

    Q_EMIT toolbar->snapClicked();
    Q_EMIT toolbar->keepMarksClicked();

    CHECK(rig.context().settings().get(fv::keys::snap_to_edges) == !edges);
    CHECK(rig.context().settings().get(fv::keys::walkthrough_keep_marks));
    Q_EMIT rig.window().walkthroughRequested();
    CHECK(fvtest::waitUntil([&rig] { return !rig.runtime().is_recording(); }));
}

TEST_CASE("during the write-up the overlay shows a dictation and a failure keeps the column",
          "[widget]") {
    fv::ui_snap::testing::ScopedFakeDesktop desktop;
    FakeSnap snap;
    fvtest::ControllerRig rig(rigWith(snap, fv::Json{{"walkthrough_write_up", true}}));
    REQUIRE(rig.startEngine());
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> writing{false};
    rig.runtime().set_walkthrough_write_up(
        [released, &writing](const fv::dictation::WalkthroughWriteUpRequest& request) {
            writing = true;
            released.wait();
            return request.result;
        });

    Q_EMIT rig.window().walkthroughRequested();
    REQUIRE(fvtest::waitUntil([&rig] { return rig.runtime().is_recording(); }));
    REQUIRE(fvtest::waitUntil([&rig] { return !rig.audio().streams().empty(); }));
    REQUIRE(rig.speak(1.0) > 0);
    Q_EMIT rig.overlay().walkthroughSnapRequested();
    REQUIRE(snap.kept);
    snap.kept(std::make_shared<fv::walkthrough::FakeShotImage>(4, 4, 0x336699));
    REQUIRE(fvtest::waitUntil(
        [&rig] { return rig.runtime().recording_walkthrough()->shot_count() == 1; }));
    Q_EMIT rig.overlay().walkthroughFinishRequested();
    REQUIRE(
        fvtest::waitUntil([&rig] { return rig.runtime().writing_up(); }, std::chrono::seconds(20)));
    auto* panel = topLevel<fv::WalkthroughPanel>();
    REQUIRE(panel);
    REQUIRE(panel->showsRecording());

    // The walkthrough's job is over: a recording now is a dictation, which the overlay shows.
    rig.overlay().showAtCursor(fv::WaveformOverlay::kStateRecording);
    CHECK(rig.overlay().isVisible());
    rig.overlay().hide();
    // A dictation that fails leaves the column, with its Cancel, until the write-up ends.
    rig.runtime().transcription_failed.emit("No speech detected");
    QTest::qWait(200);
    CHECK(panel->showsRecording());

    release.set_value();
    CHECK(fvtest::waitUntil([&rig] { return !rig.runtime().writing_up(); },
                            std::chrono::seconds(20)));
}
