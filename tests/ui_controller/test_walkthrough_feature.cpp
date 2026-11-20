#include <catch2/catch_test_macros.hpp>

#include "base/utf8.h"
#include "clipboard/testing/fake_backend.h"
#include "controller_support.h"
#include "credentials/testing/memory_backend.h"
#include "dictation/transcription_runtime.h"
#include "http/testing/fake_http_client.h"
#include "ui_history/history_feature.h"
#include "ui_history/history_sidebar.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_window.h"
#include "ui_snap/affinity.h"
#include "ui_snap/testing/fake_desktop.h"
#include "ui_walkthrough/past_walkthroughs.h"
#include "ui_walkthrough/walkthrough_library.h"
#include "ui_walkthrough/walkthrough_panel.h"
#include "ui_walkthrough/walkthrough_ready.h"
#include "ui_walkthrough/walkthrough_tab.h"
#include "ui_widgets/tabbed_content.h"
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
} // namespace

TEST_CASE("walkthrough feature connects the tab to the recording runtime", "[widget]") {
    fv::http::FakeHttpClient http;
    fv::CredentialStore credentials(std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity());
    fvtest::RigOptions options;
    options.http = &http;
    options.credentials = &credentials;
    options.walkthrough = true;
    options.platform = [](fv::ControllerPlatform& platform) {
        platform.clipboardBackend = [] {
            return std::make_unique<fv::clipboard::FakeClipboardBackend>();
        };
    };
    fvtest::ControllerRig rig(options);
    REQUIRE(rig.startEngine());
    REQUIRE(rig.context().screens().isDefined(fv::screen_key::walkthrough));
    auto* history = rig.context().service<fv::HistoryPanel>().sidebar();
    REQUIRE(history);
    REQUIRE(history->findChild<fv::PastWalkthroughsPanel*>());
    rig.window().tabbedContent()->setCurrentIndex(fv::TabbedContentWidget::TabWalkthrough);
    auto* tab = qobject_cast<fv::WalkthroughTab*>(
        rig.context().screens().existing(fv::screen_key::walkthrough));
    REQUIRE(tab);
    // A23-23 / open-items "Registering the Walkthrough tab": the storyboard keeps a dragged height.
    CHECK(rig.window().isPageElastic(tab));
    Q_EMIT tab->settingsRequested(QStringLiteral("walkthroughs"));
    auto& settingsWindow = rig.context().service<fv::SettingsWindow>();
    REQUIRE(settingsWindow.dialog());
    CHECK(settingsWindow.dialog()->currentDestination() == QStringLiteral("walkthroughs"));
    settingsWindow.dialog()->close();
    Q_EMIT tab->walkthroughRequested();
    REQUIRE(fvtest::waitUntil([&rig] { return rig.runtime().is_recording(); }));
    CHECK(rig.window().isWalkthroughActive());
    CHECK(rig.runtime().recording_walkthrough());
    Q_EMIT tab->cancelRequested();
    CHECK(fvtest::waitUntil([&rig] { return !rig.runtime().is_recording(); }));
}

TEST_CASE("walkthrough feature saves a captured screenshot and shows the ready card", "[widget]") {
    FakeSnap snap;
    fvtest::RigOptions options;
    options.walkthrough = true;
    options.walkthroughSnap = &snap;
    options.settings["walkthrough_copy_format"] = "agent_task";
    options.platform = [](fv::ControllerPlatform& platform) {
        platform.clipboardBackend = [] {
            return std::make_unique<fv::clipboard::FakeClipboardBackend>();
        };
    };
    fvtest::ControllerRig rig(options);
    REQUIRE(rig.startEngine());
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
    fv::WalkthroughReadyCard* ready = nullptr;
    REQUIRE(fvtest::waitUntil(
        [&ready] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* card = qobject_cast<fv::WalkthroughReadyCard*>(widget);
                if (card && card->isVisible()) {
                    ready = card;
                    return true;
                }
            }
            return false;
        },
        std::chrono::seconds(20)));
    REQUIRE(ready->result());
    CHECK(ready->result()->shots == 1);
    CHECK(std::filesystem::is_regular_file(ready->result()->pdf));
    REQUIRE(fvtest::waitUntil([&rig] { return rig.keys().sent().size() == 2; },
                              std::chrono::seconds(5)));
    // ui_controller.paste_walkthrough's report: the status line and the card both say it.
    const QString pasted =
        QStringLiteral("PDF and instructions pasted; review them before sending");
    REQUIRE(fvtest::waitUntil([&rig, &pasted] { return rig.window().status() == pasted; }));
    CHECK(ready->subtitleLabel()->text() == pasted);
    // Copy again reads the walkthrough from disk and says so.
    Q_EMIT ready->copyRequested(fv::qt::toQString(fv::path_to_utf8(ready->result()->folder)),
                                QStringLiteral("pdf"));
    CHECK(rig.window().status() == QStringLiteral("Walkthrough copied"));
    const auto target = rig.context().service<fv::focus::CaptureService>().current_identity();
    REQUIRE(target);
    rig.hotkeys().paste_walkthrough_requested.emit(target);
    REQUIRE(fvtest::waitUntil([&rig] { return rig.keys().sent().size() == 4; },
                              std::chrono::seconds(5)));
    Q_EMIT ready->reviewRequested(fv::qt::toQString(fv::path_to_utf8(ready->result()->folder)));
    auto* library = rig.window().findChild<fv::WalkthroughLibraryDialog*>();
    REQUIRE(library);
    CHECK(library->isVisible());
}

TEST_CASE("walkthrough shortcuts reach recording and screenshot through the fake hook",
          "[widget]") {
    FakeSnap snap;
    fvtest::RigOptions options;
    options.walkthrough = true;
    options.walkthroughSnap = &snap;
    options.settings["hotkeys"] = {{"walkthrough", "f6"}, {"screenshot", "f7"}};
    options.platform = [](fv::ControllerPlatform& platform) {
        platform.clipboardBackend = [] {
            return std::make_unique<fv::clipboard::FakeClipboardBackend>();
        };
    };
    fvtest::ControllerRig rig(options);
    REQUIRE(rig.startEngine());
    auto press = [&rig](std::string name) {
        fv::hotkeys::KeyEvent event;
        event.name = std::move(name);
        event.down = true;
        event.t = fv::hotkeys::monotonic_clock()();
        rig.hook().send(event);
        event.down = false;
        rig.hook().send(event);
    };
    press("f6");
    REQUIRE(fvtest::waitUntil([&rig] { return rig.runtime().is_recording(); }));
    press("f7");
    REQUIRE(fvtest::waitUntil([&snap] { return static_cast<bool>(snap.kept); }));
    snap.kept(std::make_shared<fv::walkthrough::FakeShotImage>(4, 4, 0x336699));
    REQUIRE(fvtest::waitUntil([&rig] {
        auto recording = rig.runtime().recording_walkthrough();
        return recording && recording->shot_count() == 1;
    }));
    rig.window().tabbedContent()->setCurrentIndex(fv::TabbedContentWidget::TabWalkthrough);
    auto* tab = qobject_cast<fv::WalkthroughTab*>(
        rig.context().screens().existing(fv::screen_key::walkthrough));
    REQUIRE(tab);
    Q_EMIT tab->cancelRequested();
    CHECK(fvtest::waitUntil([&rig] { return !rig.runtime().is_recording(); }));
}

namespace {
fv::WalkthroughPanel* walkthroughColumn() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* panel = qobject_cast<fv::WalkthroughPanel*>(widget))
            return panel;
    }
    return nullptr;
}
} // namespace

// ui_controller._show_walkthrough_panel keeps the column out of screenshots once shown, and
// _dismiss_walkthrough_panel_for_job closes it when the walkthrough is canceled (CANCELING)
// instead of leaving it on "Writing it up" (A23-62, A23-77).
TEST_CASE("the walkthrough column is kept out of screenshots and a cancel dismisses it",
          "[widget]") {
    fv::ui_snap::testing::ScopedFakeDesktop desktop;
    FakeSnap snap;
    fvtest::RigOptions options;
    options.walkthrough = true;
    options.walkthroughSnap = &snap;
    options.platform = [](fv::ControllerPlatform& platform) {
        platform.clipboardBackend = [] {
            return std::make_unique<fv::clipboard::FakeClipboardBackend>();
        };
    };
    fvtest::ControllerRig rig(options);
    REQUIRE(rig.startEngine());
    Q_EMIT rig.window().walkthroughRequested();
    REQUIRE(fvtest::waitUntil([&rig] { return rig.runtime().is_recording(); }));
    fv::WalkthroughPanel* panel = walkthroughColumn();
    REQUIRE(panel);
    REQUIRE(fvtest::waitUntil([panel] { return panel->showsRecording(); }));
    CHECK(fv::ui_snap::isKeptOut(panel));
    Q_EMIT panel->cancelRequested();
    REQUIRE(fvtest::waitUntil([&rig] { return !rig.runtime().is_recording(); }));
    CHECK(fvtest::waitUntil([panel] {
        return panel->state() == fv::WalkthroughPanel::State::Done ||
               panel->state() == fv::WalkthroughPanel::State::Hidden;
    }));
}
