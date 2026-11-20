// The Scratchpad as the record feature provides it: the service in the context, the clipboard
// Copy all writes to, the notes file in the data folder, the hotkey the feature connects and the
// triggers (the View menu and the tray) the composition connects to it (OpenWhisper's
// test_the_app_opens_it_and_dictation_lands_in_it). Everything is a fake: the clipboard is a
// FakeClipboardBackend, the keyboard hook a FakeKeyboardHook and the tray never reaches the
// notification area.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include <QApplication>
#include <QMetaMethod>
#include <QPlainTextEdit>
#include <QSystemTrayIcon>
#include <QTextCursor>

#include "app_theme.h"
#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/signal.h"
#include "base/testing/manual_executor.h"
#include "clipboard/temporary_clipboard.h"
#include "clipboard/testing/fake_backend.h"
#include "dictation/hotkey_runtime.h"
#include "dictation/recording_control.h"
#include "focus/testing/fake_synthetic_keys.h"
#include "hotkeys/hotkey_manager.h"
#include "hotkeys/testing/fake_keyboard_hook.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_record/record_feature.h"
#include "ui_record/scratchpad.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_shell/system_tray.h"
#include "ui_widgets/eliding_label.h"

namespace {

// The recording side the hotkey runtime asks, which these tests never reach.
class IdleControl final : public fv::dictation::RecordingControl {
public:
    bool is_recording() const override { return false; }
    bool recorder_busy() const override { return false; }
    bool canceling_recording() const override { return false; }
    bool capture_canceled() const override { return false; }
    bool stream_open() const override { return false; }
    bool start(std::string) override { return false; }
    void stop() override {}
    bool toggle() override { return false; }
    bool cancel() override { return false; }
    void minimize_to_tray() override {}
    void paste_last_original() override {}
    fv::Signal<bool>& recording_state_changed() override { return changed_; }

private:
    fv::Signal<bool> changed_;
};

// What Bootstrap builds for the shell and the record feature, with the services the Scratchpad
// reaches: a data folder, the settings, the persistence strand (run by the test), a clipboard and
// the hotkey runtime. The tray is made by the test.
class FeatureFixture {
public:
    FeatureFixture()
        : paths_(fv::Paths::resolve(fv::identity(), fv::system_env())), ui_(qApp),
          manager_({}, std::make_unique<fv::hotkeys::FakeKeyboardHook>()),
          runtime_(std::make_shared<fv::dictation::HotkeyRuntime>(manager_, control_, settings_,
                                                                  ui_, fv::system_clock(), keys_)),
          context_(services()) {
        auto backend = std::make_unique<fv::clipboard::FakeClipboardBackend>();
        clipboard = backend.get();
        context_.provide<fv::clipboard::TemporaryClipboard>(
            std::make_shared<fv::clipboard::TemporaryClipboard>(
                std::move(backend), fv::clipboard::SequenceSource{}, fv::clipboard::Renderer{},
                nullptr, true));
        context_.provide<fv::dictation::HotkeyRuntime>(runtime_);

        record_.provide(context_);
        shell_.provide(context_);
        record_.install(context_);
        shell_.install(context_);
        window().availableScreenRect = [] {
            return QRect(0, 0, 1920, 1080);
        };
        window().completeSetup();
    }

    fv::MainWindow& window() { return context_.service<fv::MainWindow>(); }
    fv::AppContext& context() { return context_; }
    fv::ScratchpadService& pad() { return context_.service<fv::ScratchpadService>(); }
    fv::dictation::HotkeyRuntime& hotkeys() { return *runtime_; }
    fv::Executor& ui() { return ui_; }
    std::filesystem::path notes() const { return paths_.user_file("scratchpad.txt"); }
    // The service's window is the active one.
    void focused(bool active) {
        pad().ensureWindow().setActivityProbe([active] { return active; });
    }

    fv::clipboard::FakeClipboardBackend* clipboard = nullptr;

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.ui = &ui_;
        made.persistence = &persistence_;
        return made;
    }

    fvtest::AppTheme theme_;
    fv::testkit::TempDataRoot root_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::ManualExecutor persistence_;
    fv::qt::QtUiExecutor ui_;
    IdleControl control_;
    fv::focus::FakeSyntheticKeys keys_;
    fv::hotkeys::HotkeyManager manager_;
    std::shared_ptr<fv::dictation::HotkeyRuntime> runtime_;
    fv::AppContext context_;
    fv::ShellFeature shell_;
    fv::RecordFeature record_;
};

fv::TrayEnvironment silentTray() {
    fv::TrayEnvironment environment;
    environment.available = [] {
        return false;
    };
    environment.showIcon = [](QSystemTrayIcon&) {
    };
    return environment;
}

std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("RecordFeature provides the Scratchpad service and builds no window yet", "[widget]") {
    FeatureFixture fixture;
    fv::ScratchpadService* pad = fixture.context().find<fv::ScratchpadService>();
    REQUIRE(pad != nullptr);
    CHECK(pad->window() == nullptr);
    CHECK_FALSE(pad->is_focused());
}

TEST_CASE("the feature leaves the View menu and the tray to the composition", "[widget]") {
    FeatureFixture fixture;
    fv::SystemTray tray(fixture.window(), silentTray());
    const auto windowSignal = QMetaMethod::fromSignal(&fv::MainWindow::scratchpadRequested);
    const auto traySignal = QMetaMethod::fromSignal(&fv::SystemTray::scratchpadRequested);
    CHECK_FALSE(fixture.window().hasReceivers(windowSignal));
    CHECK_FALSE(tray.hasReceivers(traySignal));
    fixture.pad().connectWindow(fixture.window());
    fixture.pad().connectTray(tray);
    CHECK(fixture.window().hasReceivers(windowSignal));
    CHECK(tray.hasReceivers(traySignal));
}

TEST_CASE("the View menu's Scratchpad shows it, and again hides it while it has focus",
          "[widget]") {
    FeatureFixture fixture;
    fixture.pad().connectWindow(fixture.window());
    QAction* action = fixture.context().menus().action(fv::menu_id::scratchpad);
    REQUIRE(action != nullptr);
    action->trigger();
    fv::ScratchpadWindow* pad = fixture.pad().window();
    REQUIRE(pad != nullptr);
    CHECK(pad->isVisible());
    fixture.focused(true);
    action->trigger();
    CHECK_FALSE(pad->isVisible());
}

TEST_CASE("the Scratchpad hotkey shows it from the dispatcher's signal", "[widget]") {
    FeatureFixture fixture;
    // RecordFeature::install connected it; connecting it again changes nothing.
    fixture.pad().connectHotkey(fixture.hotkeys(), fixture.ui());
    fixture.hotkeys().scratchpad_toggle_requested.emit();
    QApplication::processEvents();
    fv::ScratchpadWindow* pad = fixture.pad().window();
    REQUIRE(pad != nullptr);
    CHECK(pad->isVisible());
    fixture.focused(true);
    fixture.hotkeys().scratchpad_toggle_requested.emit();
    QApplication::processEvents();
    CHECK_FALSE(pad->isVisible());
}

TEST_CASE("the hotkey toggles the Scratchpad without anyone else connecting it", "[widget]") {
    FeatureFixture fixture;
    fixture.hotkeys().scratchpad_toggle_requested.emit();
    QApplication::processEvents();
    fv::ScratchpadWindow* pad = fixture.pad().window();
    REQUIRE(pad != nullptr);
    CHECK(pad->isVisible());
}

TEST_CASE("connecting a trigger twice still toggles once per press", "[widget]") {
    FeatureFixture fixture;
    fv::SystemTray tray(fixture.window(), silentTray());
    fixture.pad().connectWindow(fixture.window());
    fixture.pad().connectWindow(fixture.window());
    fixture.pad().connectTray(tray);
    fixture.pad().connectTray(tray);
    // A shown window has focus, so a press connected twice would show it and hide it again.
    fixture.focused(true);
    fv::ScratchpadWindow* pad = fixture.pad().window();
    REQUIRE(pad != nullptr);
    fixture.context().menus().action(fv::menu_id::scratchpad)->trigger();
    CHECK(pad->isVisible());
    fixture.context().menus().action(fv::menu_id::scratchpad)->trigger();
    CHECK_FALSE(pad->isVisible());
    Q_EMIT tray.scratchpadRequested();
    CHECK(pad->isVisible());
    Q_EMIT tray.scratchpadRequested();
    CHECK_FALSE(pad->isVisible());
    // The hotkey RecordFeature connected, connected again.
    fixture.pad().connectHotkey(fixture.hotkeys(), fixture.ui());
    fixture.hotkeys().scratchpad_toggle_requested.emit();
    QApplication::processEvents();
    CHECK(pad->isVisible());
}

TEST_CASE("the tray's Scratchpad shows it and hides it while it has focus", "[widget]") {
    FeatureFixture fixture;
    fv::SystemTray tray(fixture.window(), silentTray());
    fixture.pad().connectTray(tray);
    Q_EMIT tray.scratchpadRequested();
    fv::ScratchpadWindow* pad = fixture.pad().window();
    REQUIRE(pad != nullptr);
    CHECK(pad->isVisible());
    fixture.focused(true);
    Q_EMIT tray.scratchpadRequested();
    CHECK_FALSE(pad->isVisible());
}

// from tests/test_personalize_s8_scratchpad.py::test_the_app_opens_it_and_dictation_lands_in_it
TEST_CASE("the app opens it and dictation lands in it", "[widget]") {
    FeatureFixture fixture;
    fixture.pad().connectWindow(fixture.window());
    fixture.context().menus().action(fv::menu_id::scratchpad)->trigger();
    fv::ScratchpadWindow* pad = fixture.pad().window();
    REQUIRE(pad != nullptr);
    CHECK(pad->isVisible());
    fixture.focused(true);
    CHECK(fixture.pad().insert("From the hotkey"));
    CHECK(pad->text() == QStringLiteral("From the hotkey"));
    pad->hide();
    CHECK_FALSE(fixture.pad().insert("Elsewhere"));
}

TEST_CASE("Copy all writes to the application's clipboard", "[widget]") {
    FeatureFixture fixture;
    fv::ScratchpadWindow& pad = fixture.pad().ensureWindow();
    pad.editor()->setPlainText(QStringLiteral("Keep this ✓"));
    pad.copyAll();
    CHECK(pad.statusLabel()->text() == QStringLiteral("Copied"));
    CHECK(fixture.clipboard->text() == QStringLiteral("Keep this ✓"));
}

TEST_CASE("the notes live in the data folder's scratchpad.txt", "[widget]") {
    FeatureFixture fixture;
    { std::ofstream(fixture.notes(), std::ios::binary) << "Saved earlier"; }
    // An answer that arrives first still finds the saved notes loaded.
    fv::showScratchpadText(fixture.pad(), QStringLiteral("An answer"));
    fv::ScratchpadWindow* pad = fixture.pad().window();
    REQUIRE(pad != nullptr);
    CHECK(pad->text() == QStringLiteral("Saved earlier\n\nAn answer"));
    pad->editor()->moveCursor(QTextCursor::End);
    pad->editor()->insertPlainText(QStringLiteral(" and more"));
    pad->hide();
    CHECK(read(fixture.notes()) == "Saved earlier\n\nAn answer and more");
}

TEST_CASE("a context without paths or a persistence strand has no Scratchpad", "[widget]") {
    fvtest::AppTheme theme;
    fv::MemorySettingsStore settings;
    fv::qt::QtUiExecutor ui(qApp);
    fv::AppServices services;
    services.settings = &settings;
    services.ui = &ui;
    fv::AppContext context(services);
    fv::RecordFeature feature;
    feature.provide(context);
    feature.install(context);
    CHECK(context.find<fv::ScratchpadService>() == nullptr);
    CHECK(context.find<fv::RecordWidgets>() != nullptr);
}
