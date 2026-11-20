// What the UI controller and the application controller connect: the runtimes' signals to the
// UI's methods, the clipboard hooks, the History actions, where a recording says it came from,
// the tray, the overlay states and Quit. Ported from
// tests/test_personalize_foundation_controller.py, tests/test_personalize_foundation_ui_shell.py,
// tests/test_personalize_s9_paste_original.py, tests/test_personalize_fix_f1_tray_copy_original.py,
// tests/test_automation_fix_pipeline_sources.py, tests/test_deferred_screens.py and
// tests/test_bootstrap.py.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QMenu>
#include <QMimeData>

#include "clipboard/temporary_clipboard.h"
#include "controller_support.h"
#include "credentials/validate.h"
#include "focus/capture_service.h"
#include "history/manager.h"
#include "ui_controller/clipboard_service.h"
#include "ui_controller/overlay_router.h"
#include "ui_history/history_actions.h"
#include "ui_history/stats_dialog.h"
#include "ui_record/scratchpad.h"
#include "ui_widgets/downloads_label.h"
#include "ui_widgets/tabbed_content.h"

using fvtest::ControllerRig;
using fvtest::RigOptions;
using fvtest::waitUntil;
using namespace std::chrono_literals;

namespace {

// A dictation saved to History this session, as the transcription runtime saves one.
void saveDictation(ControllerRig& rig, const std::string& text,
                   const std::optional<std::string>& raw = std::nullopt) {
    auto& history = rig.context().service<fv::HistoryManager>();
    fvtest::offGui([&history, text, raw] {
        fv::AddEntry entry;
        entry.text = text;
        entry.model = "parakeet";
        entry.raw_text = raw;
        entry.source_name = "Quick Record";
        return history.add_entry(entry).has_value();
    });
}

QAction* trayAction(fv::SystemTray& tray, const QString& text) {
    for (QAction* action : tray.menu()->actions()) {
        if (action->text() == text) {
            return action;
        }
    }
    return nullptr;
}

} // namespace

// ---- tests/test_personalize_foundation_controller.py ----

// from tests/test_personalize_foundation_controller.py::test_ui_signals_reach_their_ui_methods
// (M1's signals: hands-free, a microphone switch and Paste original; the Scratchpad, language
// cycling and learned terms arrive with M2 and M4)
TEST_CASE("test_ui_signals_reach_their_ui_methods",
          "[test_personalize_foundation_controller][widget]") {
    ControllerRig rig;

    rig.hotkeys().hands_free_changed.emit(true);
    rig.app().recording_device_switched.emit("USB mic", "Laptop mic");
    rig.app().paste_last_original();

    // Each signal reaches the UI thread through its own posted hop: one event-loop turn is not
    // always enough under load.
    CHECK(waitUntil([&rig] { return rig.overlay().handsFree(); }));
    CHECK(waitUntil([&rig] {
        return rig.window().status() == QString::fromUtf8(fv::history_actions::kNothingYet);
    }));
}

// from
// tests/test_personalize_foundation_controller.py::test_the_recorder_comes_from_settings_and_reports_switches
TEST_CASE("test_the_recorder_comes_from_settings_and_reports_switches",
          "[test_personalize_foundation_controller][widget]") {
    ControllerRig rig;
    auto& recorder = rig.context().service<fv::Recorder>();

    recorder.device_switched.emit("USB mic", "Laptop mic");
    CHECK(waitUntil([&rig] {
        return rig.window().status() ==
               QStringLiteral("Microphone disconnected — continuing on Laptop mic");
    }));

    rig.app().change_audio_device();
    CHECK(waitUntil([&rig] { return rig.hasStatus("Audio device changed"); }));

    // The switch still reaches the UI after the new microphone order.
    recorder.device_switched.emit("Laptop mic", "");
    CHECK(waitUntil([&rig] {
        return rig.window().status() ==
               QStringLiteral(
                   "Microphone disconnected — continuing on the system default microphone");
    }));
}

// from
// tests/test_personalize_foundation_controller.py::test_cleanup_stops_playback_focus_capture_and_commands
// (Command Mode's cleanup arrives with M4)
TEST_CASE("test_cleanup_stops_playback_focus_capture_and_commands",
          "[test_personalize_foundation_controller][widget]") {
    ControllerRig rig;
    const auto installed = fv::focus::get_service();
    CHECK(dynamic_cast<fvtest::NamedAppCapture*>(installed.get()) != nullptr);

    rig.plan().run();

    // The focus capture service was shut down and uninstalled.
    CHECK(dynamic_cast<fvtest::NamedAppCapture*>(fv::focus::get_service().get()) == nullptr);
}

// ---- tests/test_personalize_foundation_ui_shell.py ----

// from
// tests/test_personalize_foundation_ui_shell.py::TestClipboardHooks::test_staging_without_html_keeps_the_one_argument_call
TEST_CASE("test_staging_without_html_keeps_the_one_argument_call",
          "[test_personalize_foundation_ui_shell][widget]") {
    ControllerRig rig;
    fv::ClipboardService& clipboard = *rig.feature().clipboardService();
    QGuiApplication::clipboard()->setText(QStringLiteral("mine"));

    for (const std::optional<std::string>& html :
         {std::optional<std::string>(), std::optional<std::string>("")}) {
        const fv::focus::StageResult stage = clipboard.stageTranscriptForPaste("hello", html);
        CHECK(stage.written);
        CHECK(stage.lease.has_value());
        CHECK(QGuiApplication::clipboard()->text() == QStringLiteral("hello"));
        CHECK_FALSE(QGuiApplication::clipboard()->mimeData()->hasHtml());
        clipboard.clipboard().restore_now(*stage.lease);
    }
}

// from
// tests/test_personalize_foundation_ui_shell.py::TestClipboardHooks::test_staging_with_html_passes_it_on
TEST_CASE("test_staging_with_html_passes_it_on", "[test_personalize_foundation_ui_shell][widget]") {
    ControllerRig rig;
    fv::ClipboardService& clipboard = *rig.feature().clipboardService();

    const fv::focus::StageResult stage =
        clipboard.stageTranscriptForPaste("hello", std::string("<b>hello</b>"));

    CHECK(stage.written);
    CHECK(QGuiApplication::clipboard()->mimeData()->hasHtml());
    CHECK(QGuiApplication::clipboard()->text() == QStringLiteral("hello"));
}

// from
// tests/test_personalize_foundation_ui_shell.py::TestClipboardHooks::test_staging_without_restore_passes_it_on
TEST_CASE("test_staging_without_restore_passes_it_on",
          "[test_personalize_foundation_ui_shell][widget]") {
    ControllerRig rig;
    fv::ClipboardService& clipboard = *rig.feature().clipboardService();

    const fv::focus::StageResult stage = clipboard.stageTranscriptForPaste("hello", {}, false);

    CHECK(stage.written);
    // The user's clipboard was not kept, so there is nothing to restore.
    CHECK_FALSE(stage.lease.has_value());
    CHECK_FALSE(clipboard.scheduleClipboardRestore(stage));
    CHECK(clipboard.commitTranscriptClipboard(stage, "hello"));
}

// from
// tests/test_personalize_foundation_ui_shell.py::TestClipboardHooks::test_capture_selection_hands_over_the_copy_shortcut_and_timeout
TEST_CASE("test_capture_selection_hands_over_the_copy_shortcut_and_timeout",
          "[test_personalize_foundation_ui_shell][widget]") {
    ControllerRig rig;
    fv::ClipboardService& clipboard = *rig.feature().clipboardService();
    std::vector<std::optional<std::string>> answers;
    // From the thread that asks (Command Mode's worker), the capture only hops to the GUI
    // thread: nothing is copied and nothing answered on the caller's thread.
    const auto capture = [&](std::optional<std::chrono::milliseconds> timeout) {
        const auto started = std::chrono::steady_clock::now();
        std::thread([&] {
            clipboard.captureSelection(
                [&answers](std::optional<std::string> text) { answers.push_back(std::move(text)); },
                timeout);
        }).join();
        CHECK(answers.empty());
        REQUIRE(waitUntil([&answers] { return answers.size() == 1; }, 5s));
        answers.clear();
        return std::chrono::steady_clock::now() - started;
    };

    // The default timeout is config.COMMAND_SELECTION_TIMEOUT_MS (700 ms): with nothing copied
    // the capture ends empty when it runs out.
    const auto byDefault = capture(std::nullopt);
    CHECK(byDefault >= 650ms);
    const auto given = capture(250ms);
    CHECK(given >= 200ms);
    CHECK(given < byDefault);
    // The copy shortcut is the synthetic keys' copy.
    CHECK(rig.keys().sent() == std::vector<std::string>{"copy", "copy"});
}
// from
// tests/test_personalize_foundation_ui_shell.py::TestFeatureDelegation::test_hands_free_and_device_switch_reach_the_overlay
TEST_CASE("test_hands_free_and_device_switch_reach_the_overlay",
          "[test_personalize_foundation_ui_shell][widget]") {
    ControllerRig rig;

    rig.ui().setHandsFree(true);
    CHECK(rig.overlay().handsFree());

    rig.ui().onRecordingDeviceSwitched(QStringLiteral("USB headset"), QStringLiteral("Laptop mic"));
    CHECK(rig.window().status() ==
          QStringLiteral("Microphone disconnected — continuing on Laptop mic"));
    // The caption is shown on a visible overlay only; hidden, nothing opens.
    CHECK_FALSE(rig.overlay().isVisible());
}

// from
// tests/test_personalize_foundation_ui_shell.py::TestEmptyStates::test_entry_points_do_nothing_without_their_target
TEST_CASE("test_entry_points_do_nothing_without_their_target",
          "[test_personalize_foundation_ui_shell][widget]") {
    ControllerRig rig;

    rig.ui().pasteLastOriginal();

    CHECK(waitUntil([&rig] {
        return rig.window().status() == QString::fromUtf8(fv::history_actions::kNothingYet);
    }));
    CHECK(rig.keys().sent().empty());
}

// from
// tests/test_personalize_foundation_ui_shell.py::TestAppWiring::test_settings_changes_reach_a_handler_assigned_later
TEST_CASE("test_settings_changes_reach_a_handler_assigned_later",
          "[test_personalize_foundation_ui_shell][widget]") {
    ControllerRig rig;
    rig.ui().settingsPageChanged(QStringLiteral("dictionary"));
    std::vector<std::string> kinds;
    // Assigned after the pages that call it were wired: it is read at each call.
    rig.ui().onSettingsChanged = [&kinds](std::string_view kind) {
        kinds.emplace_back(kind);
    };

    rig.ui().settingsPageChanged(QStringLiteral("hotkeys"));

    CHECK(kinds == std::vector<std::string>{"hotkeys"});
}

// from
// tests/test_personalize_foundation_ui_shell.py::TestAppWiring::test_command_listening_records_and_rewriting_cleans_up_in_their_own_looks
// (the controller's half: the tray's record item and Quick Record's activity line; the overlay's
// half is tests/ui_overlay's)
TEST_CASE("test_command_listening_records_and_rewriting_cleans_up_in_their_own_looks",
          "[test_personalize_foundation_ui_shell][widget]") {
    ControllerRig rig;
    fv::QuickRecordTab* tab = rig.quickRecord();
    REQUIRE(tab != nullptr);
    fv::OverlayRouter& router = rig.ui().overlayRouter();
    QAction* toggle = rig.tray().menu()->actions().isEmpty()
                          ? nullptr
                          : trayAction(rig.tray(), QStringLiteral("Start Recording"));
    REQUIRE(toggle != nullptr);

    router.setOverlayState(fv::appcore::OverlayState::CommandListening);
    CHECK(toggle->text() == QStringLiteral("Stop Recording"));
    CHECK(rig.overlay().isVisible());
    CHECK(rig.overlay().currentState() == fv::WaveformOverlay::kStateCommandListening);
    CHECK(tab->resolvedLabel()->text() == QStringLiteral("Listening for an edit…"));

    router.setOverlayState(fv::appcore::OverlayState::Rewriting);
    CHECK(toggle->text() == QStringLiteral("Start Recording"));
    CHECK(rig.overlay().currentState() == fv::WaveformOverlay::kStateRewriting);
    CHECK(tab->resolvedLabel()->text() == QStringLiteral("Rewriting…"));

    router.setOverlayState(fv::appcore::OverlayState::None);
    CHECK(tab->resolvedLabel()->text() != QStringLiteral("Rewriting…"));
}

// from
// tests/test_personalize_foundation_ui_shell.py::TestAppWiring::test_new_overlay_hooks_change_nothing_yet
// (the controller's half: hands-free and a caption through the router leave a hidden overlay)
TEST_CASE("test_new_overlay_hooks_change_nothing_yet",
          "[test_personalize_foundation_ui_shell][widget]") {
    ControllerRig rig;
    const QString state = rig.overlay().currentState();

    rig.ui().overlayRouter().setHandsFree(true);
    rig.ui().overlayRouter().showCaption(QStringLiteral("Switched to Laptop mic"));

    CHECK_FALSE(rig.overlay().isVisible());
    CHECK(rig.overlay().currentState() == state);
}

// ---- tests/test_personalize_s9_paste_original.py ----

// from tests/test_personalize_s9_paste_original.py::test_the_controller_gives_the_ui_its_paste
TEST_CASE("test_the_controller_gives_the_ui_its_paste",
          "[test_personalize_s9_paste_original][widget]") {
    ControllerRig rig;
    saveDictation(rig, "Hello, world.", std::string("um hello world"));

    // The Paste original shortcut reaches the runtime's paste through the UI.
    rig.app().paste_last_original();

    REQUIRE(waitUntil([&rig] { return rig.keys().sent() == std::vector<std::string>{"paste"}; }));
    CHECK(waitUntil([&rig] {
        return rig.window().status() == QString::fromUtf8(fv::history_actions::kPasted);
    }));
}

// ---- tests/test_personalize_fix_f1_tray_copy_original.py ----

// from
// tests/test_personalize_fix_f1_tray_copy_original.py::test_the_tray_item_copies_and_never_pastes
TEST_CASE("test_the_tray_item_copies_and_never_pastes",
          "[test_personalize_fix_f1_tray_copy_original][widget]") {
    ControllerRig rig;
    saveDictation(rig, "Hello, world.", std::string("um hello world"));
    QAction* copy = trayAction(rig.tray(), QStringLiteral("Copy original of last dictation"));
    REQUIRE(copy != nullptr);

    copy->trigger();

    REQUIRE(waitUntil([&rig] {
        return rig.window().status() == QString::fromUtf8(fv::history_actions::kCopied);
    }));
    CHECK(QGuiApplication::clipboard()->text() == QStringLiteral("um hello world"));
    CHECK(rig.keys().sent().empty());
    CHECK(copy->text() == QStringLiteral("Copy original of last dictation"));
}

// ---- tests/test_automation_fix_pipeline_sources.py ----

// from tests/test_automation_fix_pipeline_sources.py::test_the_window_button_records_with_source_ui
// (cleanup profiles arrive with M4, so there is no profile start)
TEST_CASE("test_the_window_button_records_with_source_ui",
          "[test_automation_fix_pipeline_sources][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());

    REQUIRE(rig.ui().startRecording());

    REQUIRE(rig.runtime().current_job() != nullptr);
    CHECK(rig.runtime().current_job()->source == "ui");
    rig.ui().cancelRecording();
}

// from
// tests/test_automation_fix_pipeline_sources.py::test_the_tray_records_with_source_ui_and_the_shell_with_dbus
// (the tray half; the desktop shell's D-Bus button is not on Windows)
TEST_CASE("test_the_tray_records_with_source_ui_and_the_shell_with_dbus",
          "[test_automation_fix_pipeline_sources][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());

    Q_EMIT rig.tray().toggleRecording();

    REQUIRE(rig.runtime().current_job() != nullptr);
    CHECK(rig.runtime().current_job()->source == "ui");
    rig.ui().cancelRecording();
}

// from tests/test_automation_fix_pipeline_sources.py::test_the_controller_passes_the_source_on
TEST_CASE("test_the_controller_passes_the_source_on",
          "[test_automation_fix_pipeline_sources][widget]") {
    const std::string source = GENERATE("ui", "dbus", "hotkey");
    ControllerRig rig;
    REQUIRE(rig.startEngine());

    REQUIRE(rig.app().start_recording(source));

    REQUIRE(rig.runtime().current_job() != nullptr);
    CHECK(rig.runtime().current_job()->source == source);
    rig.app().cancel();
}

// from
// tests/test_automation_fix_pipeline_sources.py::test_a_shortcut_still_records_with_source_hotkey
TEST_CASE("test_a_shortcut_still_records_with_source_hotkey",
          "[test_automation_fix_pipeline_sources][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());

    rig.pressKeypad("*");

    REQUIRE(waitUntil([&rig] { return rig.runtime().is_recording(); }));
    REQUIRE(rig.runtime().current_job() != nullptr);
    CHECK(rig.runtime().current_job()->source == "hotkey");
    rig.app().cancel();
}

// ---- tests/test_deferred_screens.py ----

// from tests/test_deferred_screens.py::test_controller_startup_keeps_inactive_views_unbuilt
// (M1's screens: the Upload File stand-in; Meeting Mode arrives with M7)
TEST_CASE("test_controller_startup_keeps_inactive_views_unbuilt",
          "[test_deferred_screens][widget]") {
    ControllerRig rig;
    auto& screens = rig.context().screens();
    CHECK(screens.existing(fv::ShellFeature::kUploadScreen) == nullptr);

    rig.ui().setEngineBusy(true);
    rig.ui().setDeviceInfo(QStringLiteral("CPU"), true);
    rig.ui().clearTranscriptionStats();

    CHECK(screens.existing(fv::ShellFeature::kUploadScreen) == nullptr);
    rig.window().tabbedContent()->setCurrentIndex(fv::TabbedContentWidget::TabUploadFile);
    CHECK(screens.existing(fv::ShellFeature::kUploadScreen) != nullptr);
}

// ---- tests/test_bootstrap.py ----

// Extends
// tests/test_bootstrap.py::TestBootstrap::test_main_tells_the_user_when_the_keyring_does_not_answer
// (ported by tests/ui_shell, which also shows Bootstrap handing the warning to the lifecycle):
// the controller shows it as background news, where the idle readout would be.
TEST_CASE("the unresponsive keyring is the ambient status once the window is up",
          "[test_bootstrap][widget]") {
    ControllerRig rig(RigOptions{.settings = fv::Json{{"selected_model", "api"}}});
    rig.mainWindowShown();
    REQUIRE(waitUntil([&rig] { return rig.app().startup_database_ready(); }));
    fv::QuickRecordTab* tab = rig.quickRecord();
    REQUIRE(tab != nullptr);
    fv::ShellLifecycle* lifecycle = rig.context().find<fv::ShellLifecycle>();
    REQUIRE(lifecycle != nullptr);
    tab->setStatus(QStringLiteral("Ready to record"));

    lifecycle->ambientStatus(std::string(fv::kUnresponsiveKeyringStatus));

    CHECK(tab->resolvedLabel()->text() == fv::qt::toQString(fv::kUnresponsiveKeyringStatus));

    // News nobody asked for never replaces a message that explains a problem.
    tab->setStatus(QStringLiteral("Failed to start recording: No audio device available"));
    lifecycle->ambientStatus("Update check failed");
    CHECK(tab->resolvedLabel()->text() ==
          QStringLiteral("Failed to start recording: No audio device available"));
}

// ---- M2: the tray's and the View menu's new items, the language menu ----

namespace {

QStringList menuTexts(const QMenu* menu) {
    QStringList texts;
    for (const QAction* action : menu->actions()) {
        if (!action->isSeparator()) {
            texts << action->text();
        }
    }
    return texts;
}

// The visible top-level window of type T, or null.
template <class T> T* shownWindow() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* found = qobject_cast<T*>(widget); found != nullptr && found->isVisible()) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

// from
// tests/test_personalize_foundation_ui_shell.py::TestUiShell::test_tray_offers_the_new_actions_and_routes_them
TEST_CASE("test_tray_offers_the_new_actions_and_routes_them",
          "[test_personalize_foundation_ui_shell][widget]") {
    ControllerRig rig;
    fv::SystemTray& tray = rig.tray();
    const QStringList texts = menuTexts(tray.menu());
    for (const char* text :
         {"Copy original of last dictation", "Language", "Scratchpad", "Stats"}) {
        CHECK(texts.contains(QString::fromUtf8(text)));
    }
    CHECK(texts.indexOf(QStringLiteral("Start Recording")) <
          texts.indexOf(QStringLiteral("Copy original of last dictation")));
    CHECK(texts.indexOf(QStringLiteral("Scratchpad")) < texts.indexOf(QStringLiteral("Settings")));

    // The Language submenu is filled as it opens: the language in effect, then the way to more.
    Q_EMIT tray.languageMenu()->aboutToShow();
    const QStringList languages = menuTexts(tray.languageMenu());
    REQUIRE(languages.size() == 2);
    CHECK(languages.back() == QStringLiteral("Choose languages…"));

    // Scratchpad, Stats and Copy original each reach their feature.
    auto& scratchpad = rig.context().service<fv::ScratchpadService>();
    trayAction(tray, QStringLiteral("Scratchpad"))->trigger();
    REQUIRE(scratchpad.window() != nullptr);
    CHECK(scratchpad.window()->isVisible());
    trayAction(tray, QStringLiteral("Stats"))->trigger();
    CHECK(waitUntil([] { return shownWindow<fv::StatsDialog>() != nullptr; }));
    saveDictation(rig, "Hello, world.", std::string("um hello world"));
    trayAction(tray, QStringLiteral("Copy original of last dictation"))->trigger();
    CHECK(waitUntil([&rig] {
        return rig.window().status() == QString::fromUtf8(fv::history_actions::kCopied);
    }));
    scratchpad.window()->hide();
    if (fv::StatsDialog* stats = shownWindow<fv::StatsDialog>()) {
        stats->close();
    }
}

// from
// tests/test_personalize_foundation_ui_shell.py::TestUiShell::test_view_menu_opens_the_scratchpad_and_stats
TEST_CASE("test_view_menu_opens_the_scratchpad_and_stats",
          "[test_personalize_foundation_ui_shell][widget]") {
    ControllerRig rig;
    fv::MenuRegistry& menus = rig.context().menus();
    QAction* scratchpadAction = menus.action(fv::menu_id::scratchpad);
    QAction* statsAction = menus.action(fv::menu_id::stats);
    REQUIRE(scratchpadAction != nullptr);
    REQUIRE(statsAction != nullptr);

    scratchpadAction->trigger();
    auto& scratchpad = rig.context().service<fv::ScratchpadService>();
    REQUIRE(scratchpad.window() != nullptr);
    CHECK(scratchpad.window()->isVisible());
    statsAction->trigger();
    CHECK(waitUntil([] { return shownWindow<fv::StatsDialog>() != nullptr; }));
    scratchpad.window()->hide();
    if (fv::StatsDialog* stats = shownWindow<fv::StatsDialog>()) {
        stats->close();
    }
}

TEST_CASE("the overlay chip and the shortcut cycle the dictation language", "[widget]") {
    fvtest::RigOptions options;
    // Parakeet takes English and French among others.
    options.settings["dictation_languages"] = fv::Json::array({"en", "fr"});
    ControllerRig rig(options);

    Q_EMIT rig.overlay().languageCycleRequested();
    INFO(rig.window().status().toStdString());
    CHECK(rig.window().status().startsWith(QStringLiteral("Dictation language: ")));
    const QString first = rig.window().status();

    rig.hotkeys().cycle_language_requested.emit();
    CHECK(waitUntil([&rig, first] {
        return rig.window().status().startsWith(QStringLiteral("Dictation language: ")) &&
               rig.window().status() != first;
    }));
}
