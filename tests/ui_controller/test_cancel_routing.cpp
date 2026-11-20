// The user's Cancel through the application controller's router (application_controller.py
// cancel, 1129-1142; A03-19): the recording or transcription in progress, else an AI walkthrough
// write-up still running, else "Nothing to cancel" on the status line and as a notice.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "appcore/cancel_router.h"
#include "clipboard/testing/fake_backend.h"
#include "controller_support.h"
#include "dictation/transcription_runtime.h"
#include "dictation/walkthrough_job.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_walkthrough/walkthrough_tab.h"
#include "ui_widgets/tabbed_content.h"
#include "walkthrough/runtime_ports.h"
#include "walkthrough/testing/fake_shot_image.h"
#include "walkthrough/write_up.h"

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

} // namespace

TEST_CASE("a Cancel with nothing running says Nothing to cancel", "[widget]") {
    fvtest::ControllerRig rig;
    REQUIRE(rig.startEngine());
    CHECK_FALSE(rig.app().cancel());
    CHECK(fvtest::waitUntil([&rig] { return rig.window().status() == "Nothing to cancel"; }));
}

TEST_CASE("a Cancel once the walkthrough is over stops its AI write-up", "[widget]") {
    FakeSnap snap;
    fvtest::RigOptions options;
    options.walkthrough = true;
    options.walkthroughSnap = &snap;
    options.settings["walkthrough_write_up"] = true;
    options.platform = [](fv::ControllerPlatform& platform) {
        platform.clipboardBackend = [] {
            return std::make_unique<fv::clipboard::FakeClipboardBackend>();
        };
    };
    fvtest::ControllerRig rig(options);
    REQUIRE(rig.startEngine());
    std::atomic<bool> writing = false;
    std::atomic<bool> canceled = false;
    rig.runtime().set_walkthrough_write_up(
        [&](const fv::dictation::WalkthroughWriteUpRequest& request)
            -> fv::walkthrough::WalkthroughResult {
            writing = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!request.cancel.cancelled() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            canceled = request.cancel.cancelled();
            throw fv::walkthrough::write_up::WriteUpFailed("canceled");
        });

    rig.window().tabbedContent()->setCurrentIndex(fv::TabbedContentWidget::TabWalkthrough);
    auto* tab = qobject_cast<fv::WalkthroughTab*>(
        rig.context().screens().existing(fv::screen_key::walkthrough));
    REQUIRE(tab);
    Q_EMIT tab->walkthroughRequested();
    REQUIRE(fvtest::waitUntil([&rig] { return rig.runtime().is_recording(); }));
    REQUIRE(fvtest::waitUntil([&rig] { return !rig.audio().streams().empty(); }));
    REQUIRE(rig.speak(1.0) > 0);
    Q_EMIT rig.overlay().walkthroughSnapRequested();
    REQUIRE(snap.kept);
    snap.kept(std::make_shared<fv::walkthrough::FakeShotImage>(4, 4, 0x336699));
    REQUIRE(fvtest::waitUntil(
        [&rig] { return rig.runtime().recording_walkthrough()->shot_count() == 1; }));
    Q_EMIT rig.overlay().walkthroughFinishRequested();
    REQUIRE(fvtest::waitUntil([&] { return writing.load(); }, std::chrono::seconds(20)));
    REQUIRE(rig.runtime().writing_up());

    // The recording and its job are over: the router's second step reaches the write-up.
    CHECK(rig.app().cancel());
    CHECK(fvtest::waitUntil([&] { return canceled.load(); }));
    CHECK(rig.hasStatus("Stopping the write-up..."));
    CHECK_FALSE(rig.hasStatus("Nothing to cancel"));
    CHECK(fvtest::waitUntil([&rig] { return !rig.runtime().writing_up(); }));
}
