// The walkthrough library's lease and the AI write-up through the composed app
// (application_controller.py _acquire_walkthrough_library, _acquire_backup and cancel): a library
// edit, which may run an agent for minutes, and the write-up of a finished walkthrough each hold
// the library and shut out a backup, but leave dictation free and the window enabled. The gate
// texts replay in tests/appcore/test_golden_gates.cpp; here the walkthrough feature's ReviewHost
// (walkthrough_feature.cpp host.acquire/release), the backup lifecycle and the cancel router are
// the app's own.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <QLabel>
#include <QWidget>

#include "base/utf8.h"
#include "clipboard/testing/fake_backend.h"
#include "controller_support.h"
#include "dictation/transcription_runtime.h"
#include "dictation/walkthrough_job.h"
#include "ui_controller/backup_lifecycle.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_walkthrough/walkthrough_library.h"
#include "ui_walkthrough/walkthrough_review.h"
#include "ui_walkthrough/walkthrough_tab.h"
#include "ui_widgets/tabbed_content.h"
#include "walkthrough/finish.h"
#include "walkthrough/runtime_ports.h"
#include "walkthrough/session.h"
#include "walkthrough/storage.h"
#include "walkthrough/testing/fake_shot_image.h"
#include "walkthrough/write_up.h"

namespace wt = fv::walkthrough;
using fvtest::ControllerRig;
using fvtest::waitUntil;

namespace {

class FakeHandle final : public wt::SnapHandle {
public:
    bool active() const override { return open; }
    void keep() override { open = false; }
    void drop() override { open = false; }
    std::optional<wt::Rect> capture_rect() const override { return {}; }
    bool open = true;
};

class FakeSnap final : public wt::SnapPort {
public:
    std::pair<bool, std::string> support() const override { return {true, {}}; }
    void request_access() override {}
    std::shared_ptr<wt::SnapHandle> start(wt::SnapRequest, Kept callback, Dropped,
                                          Failed) override {
        kept = std::move(callback);
        return std::make_shared<FakeHandle>();
    }
    wt::ShotImagePtr render(wt::ShotImagePtr image, std::optional<wt::Rect>,
                            wt::MarksPtr) override {
        return image;
    }
    bool window_mode_supported() const override { return true; }
    Kept kept;
};

fvtest::RigOptions walkthroughRig(wt::SnapPort* snap = nullptr) {
    fvtest::RigOptions options;
    options.walkthrough = true;
    options.walkthroughSnap = snap;
    options.platform = [](fv::ControllerPlatform& platform) {
        platform.clipboardBackend = [] {
            return std::make_unique<fv::clipboard::FakeClipboardBackend>();
        };
    };
    return options;
}

// A finished walkthrough with one screenshot and its words.
std::filesystem::path finishedWalkthrough(const std::filesystem::path& root) {
    using namespace std::chrono;
    wt::WalkthroughSession::Options options;
    options.started_at = local_seconds(local_days{year{2026} / 10 / 25} + hours{9});
    options.root = root;
    wt::WalkthroughSession session(std::move(options));
    session.add_shot(std::make_shared<wt::FakeShotImage>(120, 80, 0xf84f30), 1.0);
    const auto shots = session.saved_shots();
    const std::vector<std::string> texts{"the first step", ""};
    const auto result =
        wt::build_documents(session, shots, texts, texts, 3.0, {}, fv::Json::object());
    session.end();
    return result.folder;
}

fv::WalkthroughTab* openTab(ControllerRig& rig) {
    rig.window().tabbedContent()->setCurrentIndex(fv::TabbedContentWidget::TabWalkthrough);
    return qobject_cast<fv::WalkthroughTab*>(
        rig.context().screens().existing(fv::screen_key::walkthrough));
}

fv::WalkthroughLibraryDialog* openLibrary(ControllerRig& rig, const std::filesystem::path& folder) {
    Q_EMIT rig.window().walkthroughLibraryRequested(
        QString::fromStdString(fv::path_to_utf8(folder)));
    return rig.window().findChild<fv::WalkthroughLibraryDialog*>();
}

bool contains(const std::optional<std::string>& text, const std::string& part) {
    return text.has_value() && text->find(part) != std::string::npos;
}

// A library operation that runs until `release` is set or it is canceled.
fv::ReviewOperation heldOperation(std::atomic<bool>& started, std::atomic<bool>& release) {
    return [&started, &release](const fv::CancelToken& cancel) {
        started = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!release && !cancel.cancelled() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return fv::ReviewOutcome{};
    };
}

fv::ReviewOperation quickOperation() {
    return [](const fv::CancelToken&) {
        return fv::ReviewOutcome{};
    };
}

} // namespace

// from
// tests/test_application_controller.py::TestApplicationController::test_walkthrough_library_edit_leaves_dictation_free
TEST_CASE("a walkthrough library edit leaves dictation free and the window enabled",
          "[test_application_controller][widget]") {
    ControllerRig rig(walkthroughRig());
    REQUIRE(rig.startEngine());
    const auto folder = finishedWalkthrough(wt::captures_root(rig.paths()));
    fv::WalkthroughTab* tab = openTab(rig);
    REQUIRE(tab);
    REQUIRE(tab->review()->showFolder(folder));
    QWidget* central = rig.window().centralWidget();
    REQUIRE(central != nullptr);
    REQUIRE(central->isEnabled());
    fv::BackupLifecycle& backup = rig.context().service<fv::BackupLifecycle>();

    // A redone write-up: the tab's review takes the library through the app's gate and runs on
    // the walkthrough library's own strand.
    std::atomic<bool> started = false;
    std::atomic<bool> release = false;
    tab->review()->run(heldOperation(started, release));
    REQUIRE(tab->review()->isBusy());
    REQUIRE(waitUntil([&] { return started.load(); }));
    CHECK(rig.context().activity().snapshot().library_change);

    // The hotkey must still dictate, and the window stays usable.
    CHECK(central->isEnabled());
    CHECK_FALSE(rig.context().activity().snapshot().backup_in_progress);
    CHECK(rig.runtime().claim_job());
    rig.runtime().finish_job();

    // It still shuts out a backup and a second edit until released.
    CHECK(contains(backup.acquire(), "walkthrough"));
    CHECK_FALSE(rig.context().activity().snapshot().backup_in_progress);
    CHECK(central->isEnabled());
    fv::WalkthroughLibraryDialog* library = openLibrary(rig, folder);
    REQUIRE(library);
    REQUIRE(waitUntil([&] { return library->review()->folder() == folder; }));
    library->review()->run(quickOperation());
    CHECK_FALSE(library->review()->isBusy());
    CHECK(library->review()->notice()->text().contains(QStringLiteral("other walkthrough")));

    release = true;
    REQUIRE(waitUntil([&] { return !tab->review()->isBusy(); }));
    CHECK_FALSE(rig.context().activity().snapshot().library_change);
    CHECK_FALSE(backup.acquire().has_value());
    library->review()->run(quickOperation());
    CHECK_FALSE(library->review()->isBusy());
    CHECK(library->review()->notice()->text().contains(QStringLiteral("backup")));
    backup.release();
    CHECK(central->isEnabled());
}

// from
// tests/test_application_controller.py::TestApplicationController::test_a_walkthrough_write_up_holds_the_library_but_not_dictation
TEST_CASE("a walkthrough write-up holds the library but not dictation",
          "[test_application_controller][widget]") {
    FakeSnap snap;
    fvtest::RigOptions options = walkthroughRig(&snap);
    options.settings["walkthrough_write_up"] = true;
    ControllerRig rig(options);
    REQUIRE(rig.startEngine());
    std::atomic<bool> writing = false;
    std::atomic<bool> canceled = false;
    rig.runtime().set_walkthrough_write_up(
        [&](const fv::dictation::WalkthroughWriteUpRequest& request) -> wt::WalkthroughResult {
            writing = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!request.cancel.cancelled() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            canceled = request.cancel.cancelled();
            throw wt::write_up::WriteUpFailed("canceled");
        });

    // A walkthrough recorded with one screenshot and finished: its write-up starts.
    fv::WalkthroughTab* tab = openTab(rig);
    REQUIRE(tab);
    Q_EMIT tab->walkthroughRequested();
    REQUIRE(waitUntil([&rig] { return rig.runtime().is_recording(); }));
    REQUIRE(waitUntil([&rig] { return !rig.audio().streams().empty(); }));
    REQUIRE(rig.speak(1.0) > 0);
    Q_EMIT rig.overlay().walkthroughSnapRequested();
    REQUIRE(snap.kept);
    snap.kept(std::make_shared<wt::FakeShotImage>(4, 4, 0x336699));
    REQUIRE(waitUntil([&rig] { return rig.runtime().recording_walkthrough()->shot_count() == 1; }));
    Q_EMIT rig.overlay().walkthroughFinishRequested();
    REQUIRE(waitUntil([&] { return writing.load(); }, std::chrono::seconds(20)));
    REQUIRE(rig.runtime().writing_up());
    // The recording's job has handed over to the write-up.
    REQUIRE(waitUntil([&rig] { return !rig.context().activity().snapshot().walkthrough_job; }));

    // The write-up holds the library and shuts out a backup...
    fv::BackupLifecycle& backup = rig.context().service<fv::BackupLifecycle>();
    CHECK(contains(backup.acquire(), "walkthrough"));
    const auto root = wt::captures_root(rig.paths());
    const auto folder = finishedWalkthrough(root);
    fv::WalkthroughLibraryDialog* library = openLibrary(rig, folder);
    REQUIRE(library);
    REQUIRE(waitUntil([&] { return library->review()->folder() == folder; }));
    library->review()->run(quickOperation());
    CHECK_FALSE(library->review()->isBusy());
    CHECK(library->review()->notice()->text().contains(QStringLiteral("write-up")));
    // ...but not dictation.
    CHECK(rig.runtime().claim_job());
    rig.runtime().finish_job();
    CHECK(rig.window().centralWidget()->isEnabled());

    // The user's Cancel with nothing recorded or transcribed stops it.
    CHECK(rig.app().cancel());
    CHECK(waitUntil([&] { return canceled.load(); }));
    CHECK(waitUntil([&rig] { return !rig.runtime().writing_up(); }));
}
