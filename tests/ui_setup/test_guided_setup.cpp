// The guided setup window over the real Settings window (App pages and Recording over fakes):
// tests/test_guided_setup.py, less the eligibility tests (appcore, M2.2) and the cleanup
// provider tests (M4). The microphone test runs on audio::testing::FakeBackend; no test opens a
// real device, hook or clipboard.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <string>
#include <tuple>

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTest>

#include "appcore/guided_setup_state.h"
#include "appcore/mic_priority.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "guided_setup_support.h"
#include "hotkeys/grammar.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_record/microphone_check.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/settings_dialog.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::GuidedSetupDialog;
using fv::Json;
using fvtest::SetupRig;
namespace state = fv::appcore::guided_setup_state;
namespace dest = fv::destinations;

QString qs(std::string_view text) {
    return fv::qt::toQString(text);
}

QString product() {
    return fv::qt::toQString(fv::identity().product);
}

QString toneOf(const QWidget* widget) {
    return widget->property(fv::ui::prop::tone).toString();
}

QAbstractButton* sourceSwitch(fv::SettingsDialog& settings, std::string_view name) {
    return qobject_cast<QAbstractButton*>(settings.control(name));
}

// The Settings window's microphone list has arrived (it is read on the worker).
void waitForDevices(fv::SettingsDialog& settings) {
    auto* combo = qobject_cast<QComboBox*>(settings.control("audio_device_combo"));
    REQUIRE(combo != nullptr);
    REQUIRE(fvtest::pumpUntil([&] { return combo->count() > 2; }));
}

} // namespace

// ---- welcome page ----

// from tests/test_guided_setup.py::test_welcome_page_does_not_build_settings
TEST_CASE("test_welcome_page_does_not_build_settings", "[test_guided_setup][widget]") {
    SetupRig rig(Json::object(), true);
    GuidedSetupDialog& setup = rig.setup();
    CHECK(rig.built == 0);
    CHECK(rig.settings() == nullptr);
    CHECK(setup.page() == GuidedSetupDialog::Welcome);
    CHECK(setup.stepRow()->isHidden());
    CHECK(setup.nextButton()->text() == QStringLiteral("Get started"));
    CHECK(setup.skipButton()->text() == QStringLiteral("Not now"));
    CHECK_FALSE(rig.all().contains("guided_setup_state"));
}

// from tests/test_guided_setup.py::test_welcome_dismissal_is_durable_and_preserves_preferences
TEST_CASE("test_welcome_dismissal_is_durable_and_preserves_preferences",
          "[test_guided_setup][widget]") {
    const std::string dismiss = GENERATE("not_now", "close", "escape");
    CAPTURE(dismiss);
    SetupRig rig(Json{{"transcript_cleanup_prompt", "My exact prompt"}, {"auto_paste", false}},
                 true);
    GuidedSetupDialog& setup = rig.setup();
    Json before = rig.all();
    setup.show();
    if (dismiss == "not_now") {
        setup.skipButton()->click();
    } else if (dismiss == "close") {
        setup.close();
    } else {
        QTest::keyClick(&setup, Qt::Key_Escape);
    }
    CHECK_FALSE(setup.isVisible());
    before["guided_setup_state"] = "skipped";
    CHECK(rig.all() == before);
    CHECK_FALSE(state::should_invite(rig.store()));
}

// from tests/test_guided_setup.py::test_get_started_records_progress_and_binds_settings_once
TEST_CASE("test_get_started_records_progress_and_binds_settings_once",
          "[test_guided_setup][widget]") {
    SetupRig rig(Json::object(), true);
    GuidedSetupDialog& setup = rig.setup();
    setup.nextButton()->click();
    CHECK(setup.page() == GuidedSetupDialog::Record);
    CHECK(rig.saved("guided_setup_state") == "started");
    fv::SettingsDialog* first = rig.settings();
    REQUIRE(first != nullptr);
    setup.showPage(GuidedSetupDialog::CleanUp);
    CHECK(rig.settings() == first);
    CHECK(rig.built == 1);
}

// from tests/test_guided_setup.py::test_failed_dismissal_keeps_welcome_reviewable_for_retry
TEST_CASE("test_failed_dismissal_keeps_welcome_reviewable_for_retry",
          "[test_guided_setup][widget]") {
    SetupRig rig(Json::object(), true);
    GuidedSetupDialog& setup = rig.setup();
    setup.show();
    rig.store().file().fail_next_replace("Disk full");
    setup.skipButton()->click();
    CHECK(setup.isVisible());
    CHECK(setup.errorLabel()->text().contains(QStringLiteral("Disk full")));
    CHECK(setup.errorLabel()->text().startsWith(QStringLiteral("Couldn't save setup progress: ")));
    setup.skipButton()->click();
    CHECK_FALSE(setup.isVisible());
}

// ---- steps ----

// from tests/test_guided_setup.py::test_open_and_continue_preserve_all_choices_and_custom_prompt
TEST_CASE("test_open_and_continue_preserve_all_choices_and_custom_prompt",
          "[test_guided_setup][widget]") {
    SetupRig rig(Json{
        {"transcript_cleanup_enabled", false},
        {"transcript_cleanup_prompt", "Keep my exact wording"},
        {"transcript_cleanup_provider", "openai"},
        {"transcript_cleanup_model", "gpt-4o-mini"},
        {"auto_paste", false},
        {"streaming_enabled", false},
        {"hotkeys", Json{{"record_toggle", "ctrl+alt+j"}}},
    });
    GuidedSetupDialog& setup = rig.setup();
    Json before = rig.all();
    CHECK(before["guided_setup_state"] == "started");
    CHECK(setup.backButton()->isHidden());
    setup.nextButton()->click();
    CHECK(setup.page() == GuidedSetupDialog::CleanUp);
    CHECK(setup.cleanupOptions()->isHidden());
    CHECK(setup.cleanupDetail()->text().contains(QStringLiteral("no text is sent")));
    setup.nextButton()->click();
    CHECK(setup.page() == GuidedSetupDialog::Output);
    CHECK(setup.nextButton()->text() == QStringLiteral("Finish"));
    CHECK(rig.all() == before);
    CHECK_FALSE(rig.settings()->isBuilt(dest::cleanup));
    CHECK_FALSE(rig.settings()->isBuilt(dest::general));
    setup.nextButton()->click();
    CHECK(setup.page() == GuidedSetupDialog::Ready);
    before["guided_setup_state"] = "completed";
    CHECK(rig.all() == before);
    CHECK_FALSE(state::should_invite(rig.store()));
}

// from tests/test_guided_setup.py::test_failed_save_restores_projection_and_source_for_retry
TEST_CASE("test_failed_save_restores_projection_and_source_for_retry",
          "[test_guided_setup][widget]") {
    SetupRig rig(Json{{"auto_paste", false}});
    GuidedSetupDialog& setup = rig.setup();
    setup.showPage(GuidedSetupDialog::Output);
    rig.store().file().fail_next_replace("Disk full");
    setup.pasteSwitch()->click();
    CHECK_FALSE(setup.pasteSwitch()->isChecked());
    QAbstractButton* source = sourceSwitch(*rig.settings(), "auto_paste_check");
    REQUIRE(source != nullptr);
    CHECK_FALSE(source->isChecked());
    CHECK(setup.errorLabel()->text().contains(QStringLiteral("Disk full")));
    setup.pasteSwitch()->click();
    CHECK(setup.pasteSwitch()->isChecked());
    CHECK(rig.saved("auto_paste") == true);
    CHECK(setup.errorLabel()->isHidden());
}

// from tests/test_guided_setup.py::test_microphone_keeps_backups_and_invokes_live_handler
TEST_CASE("test_microphone_keeps_backups_and_invokes_live_handler", "[test_guided_setup][widget]") {
    SetupRig rig(Json{{"audio_input_priority", Json::array({fvtest::kSnowball})}});
    GuidedSetupDialog& setup = rig.setup();
    waitForDevices(*rig.settings());
    setup.refresh();
    CHECK(setup.microphoneDetail()->text().endsWith(QStringLiteral("hears you.")));
    QSignalSpy changed(&rig.pages().pages().signals(),
                       &fv::SettingsPagesSignals::audioDeviceChanged);
    int usb = -1;
    for (int index = 0; index < setup.microphoneCombo()->count(); ++index) {
        if (setup.microphoneCombo()->itemText(index).contains(QStringLiteral("USB Audio"))) {
            usb = index;
            break;
        }
    }
    REQUIRE(usb >= 0);
    setup.microphoneCombo()->setCurrentIndex(usb);
    CHECK(rig.saved("audio_input_priority") == Json::array({fvtest::kUsb, fvtest::kSnowball}));
    REQUIRE(changed.count() == 1);
    CHECK(changed.at(0).at(0).value<Json>() == Json::array({fvtest::kUsb, fvtest::kSnowball}));
    // Two saved now: the backup sentence joins the detail.
    setup.refresh();
    CHECK(setup.microphoneDetail()->text() ==
          QStringLiteral("The one %1 listens to. Test it to see that it hears you. Your backup "
                         "microphones stay as they are.")
              .arg(product()));
}

// from
// tests/test_guided_setup.py::test_shortcut_conflict_and_capture_cancellation_use_existing_path
TEST_CASE("test_shortcut_conflict_and_capture_cancellation_use_existing_path",
          "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    QSignalSpy suspended(rig.settings(), &fv::SettingsDialog::hotkeyCaptureSuspended);
    const Json before = rig.hotkeys();
    setup.shortcutInput()->beginCapture();
    setup.showPage(GuidedSetupDialog::CleanUp);
    CHECK_FALSE(setup.shortcutInput()->isCapturing());
    REQUIRE(suspended.count() >= 2);
    CHECK(suspended.first().at(0).toBool());
    CHECK_FALSE(suspended.last().at(0).toBool());
    // "kp -" is Cancel's default shortcut.
    Q_EMIT setup.shortcutInput()->captured(QStringLiteral("kp -"));
    CHECK(rig.hotkeys() == before);
    CHECK(setup.errorLabel()->text().contains(QStringLiteral("already used")));
    Q_EMIT setup.shortcutInput()->captured(QStringLiteral("ctrl+alt+j"));
    CHECK(rig.hotkeys()["record_toggle"] == "ctrl+alt+j");
    CHECK(setup.errorLabel()->isHidden());
}

// from tests/test_guided_setup.py::test_output_uses_existing_streaming_migration_and_callback
TEST_CASE("test_output_uses_existing_streaming_migration_and_callback",
          "[test_guided_setup][widget]") {
    SetupRig rig(Json{{"streaming_enabled", false}, {"streaming_overlay_enabled", true}});
    GuidedSetupDialog& setup = rig.setup();
    QSignalSpy changed(&rig.pages().pages().signals(),
                       &fv::SettingsPagesSignals::streamingSettingsChanged);
    setup.showPage(GuidedSetupDialog::Output);
    setup.liveSwitch()->click();
    CHECK(rig.saved("streaming_enabled") == true);
    CHECK_FALSE(rig.all().contains("streaming_overlay_enabled"));
    CHECK(changed.count() == 1);
}

// from tests/test_guided_setup.py::test_all_settings_and_change_links_use_stable_destinations
TEST_CASE("test_all_settings_and_change_links_use_stable_destinations",
          "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    setup.speechChangeButton()->click();
    REQUIRE(rig.destinations.size() == 1);
    CHECK(rig.destinations.front() == qs(dest::voice_model));
    CHECK(setup.page() == GuidedSetupDialog::Record);
    setup.allSettingsButton()->click();
    CHECK(rig.allSettingsOpened == 1);
    CHECK(rig.saved("guided_setup_state") == "skipped");
}

// from tests/test_guided_setup.py::test_speech_engine_status_is_honest_and_links_to_the_model_page
TEST_CASE("test_speech_engine_status_is_honest_and_links_to_the_model_page",
          "[test_guided_setup][widget]") {
    const bool ready = GENERATE(true, false);
    CAPTURE(ready);
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    rig.speech.ready = ready;
    setup.showPage(GuidedSetupDialog::Record);
    const QString line = setup.speechSummary()->text();
    CHECK(line.contains(QStringLiteral("Downloaded and ready")) == ready);
    CHECK(line.contains(QStringLiteral("Not downloaded yet")) == !ready);
    CHECK(setup.speechDownloadButton()->isHidden() == ready);
    setup.speechChangeButton()->click();
    CHECK(rig.destinations.back() == qs(dest::voice_model));
    setup.showPage(GuidedSetupDialog::Ready);
    CHECK(setup.readySpeech()->isHidden() == ready);
}

// from tests/test_guided_setup.py::test_reactivation_projects_changes_from_all_settings
TEST_CASE("test_reactivation_projects_changes_from_all_settings", "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    rig.store().mutate([](Json& settings) {
        settings["transcript_cleanup_enabled"] = true;
        settings["auto_paste"] = false;
        settings["transcript_cleanup_level"] = "high";
    });
    setup.refresh();
    CHECK(setup.cleanupSwitch()->isChecked());
    CHECK_FALSE(setup.pasteSwitch()->isChecked());
    CHECK(setup.levels()->buttons()[2]->isChecked());
}

// ---- the ready page ----

// from tests/test_guided_setup.py::test_ready_page_shows_the_shortcut_and_takes_dictation
TEST_CASE("test_ready_page_shows_the_shortcut_and_takes_dictation", "[test_guided_setup][widget]") {
    SetupRig rig(Json{{"hotkeys", Json{{"record_toggle", "ctrl+alt+j"}}}});
    GuidedSetupDialog& setup = rig.setup();
    setup.show();
    setup.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&setup));
    setup.showPage(GuidedSetupDialog::Output);
    setup.nextButton()->click();
    QTest::qWait(20);
    CHECK(setup.page() == GuidedSetupDialog::Ready);
    const QString shown =
        qs(fv::hotkeys::format_hotkey_display("ctrl+alt+j", fv::hotkeys::platform_vocabulary()));
    CHECK(setup.keycap()->text() == shown);
    CHECK(setup.tryBox()->placeholderText().contains(shown));
    CHECK(setup.property(fv::ui::prop::openwhisperCppHotkeyWindow).toBool());
    CHECK(setup.tryBox()->hasFocus());
    CHECK(setup.nextButton()->text() == QStringLiteral("Done"));
    CHECK(setup.skipButton()->isHidden());
    setup.nextButton()->click();
    CHECK_FALSE(setup.isVisible());
    CHECK(rig.saved("guided_setup_state") == "completed");
}

// from tests/test_guided_setup.py::test_ready_page_explains_where_the_try_it_text_goes
TEST_CASE("test_ready_page_explains_where_the_try_it_text_goes", "[test_guided_setup][widget]") {
    const auto [paste, copy, expected] = GENERATE(table<bool, bool, const char*>({
        {true, true, "wherever the cursor is"},
        {false, true, "V after you speak"},
        {false, false, "only appears in History"},
    }));
    CAPTURE(paste, copy);
    SetupRig rig(Json{{"auto_paste", paste}, {"copy_clipboard", copy}});
    GuidedSetupDialog& setup = rig.setup();
    setup.showPage(GuidedSetupDialog::Ready);
    CHECK(setup.tryNote()->text().contains(QString::fromUtf8(expected)));
}

// from tests/test_guided_setup.py::test_closing_a_finished_setup_keeps_it_completed
TEST_CASE("test_closing_a_finished_setup_keeps_it_completed", "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    setup.show();
    setup.showPage(GuidedSetupDialog::Output);
    setup.nextButton()->click();
    setup.close();
    CHECK(rig.saved("guided_setup_state") == "completed");
    setup.begin(false);
    setup.show();
    setup.skipButton()->click();
    CHECK(rig.saved("guided_setup_state") == "completed");
}

// from tests/test_guided_setup.py::test_ready_page_names_what_is_still_missing
// The provider summary is M4's; a stand-in that needs a key, as OpenRouter without one does.
TEST_CASE("test_ready_page_names_what_is_still_missing", "[test_guided_setup][widget]") {
    SetupRig rig(
        Json{{"transcript_cleanup_enabled", true}, {"transcript_cleanup_provider", "openrouter"}});
    GuidedSetupDialog& setup = rig.setup();
    setup.setProviderSummary([](const Json&) {
        return fv::ProviderSummary{QStringLiteral("OpenRouter"), QStringLiteral("API key needed"),
                                   QStringLiteral("Transcript text is sent to OpenRouter when "
                                                  "cleanup is on. No audio is sent for cleanup."),
                                   true, true};
    });
    rig.speech.ready = false;
    setup.showPage(GuidedSetupDialog::Ready);
    CHECK(setup.readyTitle()->text() == QStringLiteral("Almost ready"));
    CHECK_FALSE(setup.readySpeech()->isHidden());
    CHECK_FALSE(setup.readyCleanup()->isHidden());
    CHECK(setup.readyCleanupNote()->text().contains(QStringLiteral("without cleanup")));
    rig.speech.ready = true;
    setup.showPage(GuidedSetupDialog::CleanUp);
    setup.cleanupSwitch()->click();
    setup.showPage(GuidedSetupDialog::Ready);
    CHECK(setup.readyTitle()->text() == QStringLiteral("You're ready"));
    CHECK(setup.readySpeech()->isHidden());
    CHECK(setup.readyCleanup()->isHidden());
}

// from
// tests/test_guided_setup.py::test_ready_page_confirms_a_dictation_and_shows_text_that_was_not_pasted
TEST_CASE("test_ready_page_confirms_a_dictation_and_shows_text_that_was_not_pasted",
          "[test_guided_setup][widget]") {
    const bool pasted = GENERATE(true, false);
    CAPTURE(pasted);
    SetupRig rig(Json{{"auto_paste", false}});
    GuidedSetupDialog& setup = rig.setup();
    setup.show();
    setup.showPage(GuidedSetupDialog::Ready);
    setup.dictationReceived(QStringLiteral("Hello there."));
    CHECK(setup.tryFillTimer()->isActive());
    CHECK(setup.tryFillTimer()->interval() == GuidedSetupDialog::kTryFillQuickMs);
    if (pasted) {
        setup.tryBox()->setPlainText(QStringLiteral("Hello there."));
    }
    fvtest::expire(setup.tryFillTimer());
    setup.tryFillTimer()->stop();
    REQUIRE(fvtest::waitForQueuedRefresh(setup));
    CHECK(setup.tryBox()->toPlainText() == QStringLiteral("Hello there."));
    CHECK(setup.tryNote()->text().contains(QStringLiteral("That worked")));
    CHECK(setup.tryNote()->text().contains(QStringLiteral("clipboard too")) == !pasted);
    CHECK(toneOf(setup.tryNote()) == QStringLiteral("ready"));
    setup.dictationFailed(QStringLiteral("No speech detected"));
    REQUIRE(fvtest::waitForQueuedRefresh(setup));
    CHECK(setup.tryNote()->text().contains(QStringLiteral("No speech detected")));
    CHECK(toneOf(setup.tryNote()) == QStringLiteral("attention"));
    setup.begin(false);
    setup.showPage(GuidedSetupDialog::Ready);
    CHECK_FALSE(setup.tryNote()->text().contains(QStringLiteral("That worked")));
    CHECK(setup.tryBox()->toPlainText().isEmpty());
}

// from tests/test_guided_setup.py::test_dictation_elsewhere_does_not_touch_the_try_box
TEST_CASE("test_dictation_elsewhere_does_not_touch_the_try_box", "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    setup.show();
    setup.showPage(GuidedSetupDialog::Output);
    setup.dictationReceived(QStringLiteral("Hello there."));
    setup.showPage(GuidedSetupDialog::Ready);
    CHECK_FALSE(setup.tryFillTimer()->isActive());
    CHECK(setup.tryBox()->toPlainText().isEmpty());
    CHECK_FALSE(setup.tryNote()->text().contains(QStringLiteral("That worked")));
}

TEST_CASE("a dictation on the Ready page says the product is set up and waits for a paste",
          "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    setup.show();
    setup.showPage(GuidedSetupDialog::Ready);
    setup.dictationReceived(QStringLiteral("Hello there."));
    REQUIRE(fvtest::waitForQueuedRefresh(setup));
    CHECK(setup.tryNote()->text() == QStringLiteral("That worked. %1 is set up.").arg(product()));
    CHECK(setup.tryFillTimer()->interval() == GuidedSetupDialog::kTryFillMs);
    // An empty transcript and a hidden window change nothing.
    setup.dictationFailed(QString());
    REQUIRE(fvtest::waitForQueuedRefresh(setup));
    CHECK(setup.tryNote()->text() == QStringLiteral("That one didn't come through. Try again."));
}

// ---- the record step ----

// from
// tests/test_guided_setup.py::test_microphone_test_shows_a_level_and_closes_when_the_step_is_left
TEST_CASE("test_microphone_test_shows_a_level_and_closes_when_the_step_is_left",
          "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    setup.show();
    setup.micTestButton()->click();
    CHECK(setup.micTestButton()->text() == QStringLiteral("Stop"));
    CHECK(setup.micCheck()->status()->text().contains(QStringLiteral("Nothing is recorded")));
    CHECK(rig.microphone().backend->open_streams() == 1);
    setup.micCheck()->applyLevel(0.001f);
    CHECK_FALSE(setup.micCheck()->status()->text().contains(QStringLiteral("works")));
    setup.micCheck()->applyLevel(0.08f);
    CHECK(setup.micCheck()->bar()->value() > 30);
    CHECK(setup.micCheck()->status()->text().contains(QStringLiteral("works")));
    setup.showPage(GuidedSetupDialog::CleanUp);
    CHECK(rig.microphone().backend->open_streams() == 0);
    CHECK(setup.micTestButton()->text() == QStringLiteral("Test"));
    CHECK_FALSE(setup.micCheck()->isRunning());
}

// from
// tests/test_guided_setup.py::test_microphone_test_reports_silence_and_a_microphone_that_will_not_open
TEST_CASE("test_microphone_test_reports_silence_and_a_microphone_that_will_not_open",
          "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    setup.show();
    setup.micTestButton()->click();
    fvtest::expire(setup.micCheck()->quietTimer());
    CHECK(setup.micCheck()->status()->text().contains(QStringLiteral("Not hearing anything")));
    setup.micTestButton()->click();
    CHECK(rig.microphone().backend->open_streams() == 0);
    CHECK(setup.micCheck()->status()->text().contains(QStringLiteral("Didn't hear anything")));
    // No device at all: the recorder's own reason is shown.
    rig.microphone().backend->set_layout({}, {}, -1);
    setup.micTestButton()->click();
    CHECK(setup.micCheck()->status()->text() ==
          QStringLiteral("Couldn't open the microphone: No audio device available."));
    CHECK(setup.micTestButton()->text() == QStringLiteral("Test"));
    CHECK_FALSE(setup.micCheck()->isRunning());
}

// from
// tests/test_guided_setup.py::test_press_or_hold_saves_through_settings_and_restores_a_failed_save
TEST_CASE("test_press_or_hold_saves_through_settings_and_restores_a_failed_save",
          "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    QSignalSpy changed(&rig.pages().pages().signals(),
                       &fv::SettingsPagesSignals::recordingTriggerModeChanged);
    CHECK(setup.triggerCombo()->currentData().toString() == QStringLiteral("toggle"));
    rig.store().file().fail_next_replace("Disk full");
    setup.triggerCombo()->setCurrentIndex(1);
    CHECK(setup.triggerCombo()->currentData().toString() == QStringLiteral("toggle"));
    auto* source = qobject_cast<QComboBox*>(rig.settings()->control("record_mode_combo"));
    REQUIRE(source != nullptr);
    CHECK(source->currentData().toString() == QStringLiteral("toggle"));
    CHECK(setup.errorLabel()->text().contains(QStringLiteral("Disk full")));
    setup.triggerCombo()->setCurrentIndex(1);
    CHECK(rig.saved("recording_trigger_mode") == "push_hold");
    REQUIRE(changed.count() == 1);
    CHECK(changed.at(0).at(0).toString() == QStringLiteral("push_hold"));
    CHECK(setup.shortcutDetail()->text().contains(QStringLiteral("Hold it while you speak")));
    CHECK(setup.errorLabel()->isHidden());
}

// from tests/test_guided_setup.py::test_record_step_tells_laptop_users_how_to_leave_the_number_pad
TEST_CASE("test_record_step_tells_laptop_users_how_to_leave_the_number_pad",
          "[test_guided_setup][widget]") {
    SetupRig rig(Json{{"hotkeys", Json{{"record_toggle", "kp *"}}}});
    GuidedSetupDialog& setup = rig.setup();
    CHECK(setup.shortcutDetail()->text().contains(QStringLiteral("No number pad? Choose Change.")));
    setup.showPage(GuidedSetupDialog::Ready);
    CHECK_FALSE(setup.keycapHelp()->text().contains(QStringLiteral("Choose Change")));
}

// ---- the speech download ----

namespace {

// The downloading fixture: Parakeet missing, the window shown on Record.
void downloading(SetupRig& rig) {
    rig.speech.model = "parakeet-v3";
    rig.speech.ready = false;
    rig.setup().show();
    rig.setup().showPage(GuidedSetupDialog::Record);
}

} // namespace

// from
// tests/test_guided_setup.py::test_download_runs_in_place_with_progress_through_model_and_runtime
TEST_CASE("test_download_runs_in_place_with_progress_through_model_and_runtime",
          "[test_guided_setup][widget]") {
    SetupRig rig;
    downloading(rig);
    GuidedSetupDialog& setup = rig.setup();
    setup.speechDownloadButton()->click();
    REQUIRE(rig.downloads.size() == 1);
    CHECK(rig.downloads.front() == QStringLiteral("parakeet-v3"));
    CHECK(rig.destinations.empty());
    setup.modelDownloadStarted(QStringLiteral("tiny.en"));
    CHECK(setup.speechProgress()->isHidden());
    setup.modelDownloadStarted(QStringLiteral("parakeet-v3"));
    CHECK(setup.showsModelDownload(QStringLiteral("parakeet-v3")));
    setup.modelDownloadProgress(QStringLiteral("parakeet-v3"), 300'000'000, 1'200'000'000);
    CHECK(setup.speechProgress()->value() == 25);
    CHECK(setup.speechSummary()->text().contains(QStringLiteral("300 MB of 1.20 GB")));
    CHECK(setup.speechDownloadButton()->isHidden());
    CHECK(setup.showsRuntimeInstall(QStringLiteral("asr-nvidia-cpu")));
    setup.runtimeInstallProgress(QStringLiteral("asr-nvidia-cpu"), 0, 0);
    setup.modelDownloadFinished(QStringLiteral("parakeet-v3"), true);
    CHECK(setup.speechSummary()->text().contains(QStringLiteral("Installing")));
    CHECK_FALSE(setup.speechProgress()->isHidden());
    setup.showPage(GuidedSetupDialog::Ready);
    CHECK(setup.readyTitle()->text() == QStringLiteral("Almost ready"));
    CHECK_FALSE(setup.readyProgress()->isHidden());
    CHECK(setup.readyDownloadButton()->isHidden());
    rig.speech.ready = true;
    setup.runtimeInstallFinished(QStringLiteral("asr-nvidia-cpu"), true, QString());
    CHECK(setup.readyTitle()->text() == QStringLiteral("You're ready"));
    CHECK(setup.readySpeech()->isHidden());
    CHECK(setup.readyProgress()->isHidden());
    CHECK_FALSE(setup.isExpectingDownload());
}

// from tests/test_guided_setup.py::test_failed_download_says_so_and_can_be_retried
TEST_CASE("test_failed_download_says_so_and_can_be_retried", "[test_guided_setup][widget]") {
    SetupRig rig;
    downloading(rig);
    GuidedSetupDialog& setup = rig.setup();
    setup.speechDownloadButton()->click();
    setup.modelDownloadStarted(QStringLiteral("parakeet-v3"));
    setup.modelDownloadFinished(QStringLiteral("parakeet-v3"), false);
    CHECK(setup.speechSummary()->text().contains(QStringLiteral("didn't finish")));
    CHECK(setup.speechProgress()->isHidden());
    CHECK_FALSE(setup.speechDownloadButton()->isHidden());
    setup.speechDownloadButton()->click();
    CHECK(rig.downloads.size() == 2);
    CHECK_FALSE(setup.speechSummary()->text().contains(QStringLiteral("didn't finish")));
}

TEST_CASE("a failed runtime install shows the runtime's message or a plain retry",
          "[test_guided_setup][widget]") {
    SetupRig rig;
    downloading(rig);
    GuidedSetupDialog& setup = rig.setup();
    setup.speechDownloadButton()->click();
    setup.runtimeInstallProgress(QStringLiteral("asr-nvidia-cpu"), 5, 10);
    CHECK(setup.speechProgress()->value() == 50);
    CHECK(setup.speechSummary()->text().endsWith(
        QStringLiteral(u"· Installing what the model runs on…")));
    setup.runtimeInstallFinished(QStringLiteral("asr-nvidia-cpu"), false, QString());
    CHECK(setup.speechSummary()->text().endsWith(
        QStringLiteral(u"· The install didn't finish. Try again.")));
    CHECK_FALSE(setup.isExpectingDownload());
}

// from
// tests/test_guided_setup.py::test_download_without_a_local_model_or_downloader_opens_the_model_page
TEST_CASE("test_download_without_a_local_model_or_downloader_opens_the_model_page",
          "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    rig.speech.ready = false;
    setup.showPage(GuidedSetupDialog::Ready);
    setup.setDownloadModel({});
    setup.readyDownloadButton()->click();
    REQUIRE_FALSE(rig.destinations.empty());
    CHECK(rig.destinations.back() == qs(dest::voice_model));
    CHECK(rig.downloads.empty());
}

// from tests/test_guided_setup.py::test_downloads_started_elsewhere_are_not_claimed
TEST_CASE("test_downloads_started_elsewhere_are_not_claimed", "[test_guided_setup][widget]") {
    SetupRig rig;
    downloading(rig);
    GuidedSetupDialog& setup = rig.setup();
    setup.modelDownloadStarted(QStringLiteral("parakeet-v3"));
    setup.runtimeInstallProgress(QStringLiteral("asr-nvidia-cpu"), 1, 2);
    CHECK(setup.speechProgress()->isHidden());
    CHECK_FALSE(setup.showsRuntimeInstall(QStringLiteral("asr-nvidia-cpu")));
    CHECK_FALSE(setup.showsModelDownload(QStringLiteral("parakeet-v3")));
}

TEST_CASE("only a speech runtime counts as the install this window shows",
          "[test_guided_setup][widget]") {
    SetupRig rig;
    downloading(rig);
    GuidedSetupDialog& setup = rig.setup();
    setup.speechDownloadButton()->click();
    CHECK(setup.showsRuntimeInstall(QStringLiteral("asr-nvidia-cuda")));
    CHECK_FALSE(setup.showsRuntimeInstall(QStringLiteral("onnxruntime-cpu")));
    // A request the user declined (the window closed) claims nothing later.
    setup.hide();
    CHECK_FALSE(setup.isExpectingDownload());
}

// ---- getting around ----

// from tests/test_guided_setup.py::test_steps_jump_and_enter_continues
TEST_CASE("test_steps_jump_and_enter_continues", "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    setup.show();
    setup.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&setup));
    QTest::qWait(20);
    CHECK(setup.nextButton()->isDefault());
    CHECK(setup.focusWidget() == setup.nextButton());
    setup.stepButtons()[2]->click();
    CHECK(setup.page() == GuidedSetupDialog::Output);
    setup.stepButtons()[0]->click();
    CHECK(setup.page() == GuidedSetupDialog::Record);
    QTest::keyClick(&setup, Qt::Key_Return);
    CHECK(setup.page() == GuidedSetupDialog::CleanUp);
    CHECK(rig.saved("guided_setup_state") == "started");
}

TEST_CASE("the step buttons say where the user is", "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    setup.showPage(GuidedSetupDialog::Output);
    const auto& steps = setup.stepButtons();
    CHECK(steps[0]->text() == QStringLiteral(u"✓  Record"));
    CHECK(steps[1]->text() == QStringLiteral(u"✓  Clean up"));
    CHECK(steps[2]->text() == QStringLiteral("3  Output"));
    CHECK(steps[2]->property(fv::ui::prop::active).toBool());
    CHECK(steps[0]->property("done").toBool());
    CHECK(steps[1]->accessibleName() == QStringLiteral("Step 2 of 3: Clean up"));
    CHECK_FALSE(setup.backButton()->isHidden());
    setup.showPage(99);
    CHECK(setup.page() == GuidedSetupDialog::Ready);
    setup.showPage(-3);
    CHECK(setup.page() == GuidedSetupDialog::Welcome);
}

// from tests/test_guided_setup.py::test_a_finished_setup_offers_close_instead_of_skip
TEST_CASE("test_a_finished_setup_offers_close_instead_of_skip", "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    CHECK(setup.skipButton()->text() == QStringLiteral("Skip setup"));
    setup.showPage(GuidedSetupDialog::Output);
    setup.nextButton()->click();
    setup.begin(false);
    CHECK(setup.skipButton()->text() == QStringLiteral("Close"));
}

// ---- layout ----

// from tests/test_guided_setup.py::test_all_pages_fit_narrow_and_large_font_layouts
TEST_CASE("test_all_pages_fit_narrow_and_large_font_layouts", "[test_guided_setup][widget]") {
    const auto [width, scale] = GENERATE(table<int, int>({{720, 100}, {540, 130}}));
    CAPTURE(width, scale);
    fvtest::AppTheme theme(QStringLiteral("dark"), scale);
    SetupRig rig(Json{{"transcript_cleanup_enabled", true}});
    GuidedSetupDialog& setup = rig.setup();
    setup.show();
    setup.resize(width, 540);
    for (int page = GuidedSetupDialog::Welcome; page <= GuidedSetupDialog::Ready; ++page) {
        CAPTURE(page);
        setup.showPage(page);
        QTest::qWait(30);
        CHECK(setup.scrollArea()->horizontalScrollBar()->maximum() == 0);
        CHECK(setup.nextButton()->isVisible());
        CHECK(setup.nextButton()->geometry().bottom() < setup.height());
        QWidget* viewport = setup.scrollArea()->viewport();
        for (QWidget* control : setup.pages()->currentWidget()->findChildren<QWidget*>()) {
            if (qobject_cast<QAbstractButton*>(control) == nullptr &&
                qobject_cast<QComboBox*>(control) == nullptr &&
                qobject_cast<QLineEdit*>(control) == nullptr) {
                continue;
            }
            if (!control->isVisible()) {
                continue;
            }
            CAPTURE(control->objectName(), control->accessibleName());
            const int left = control->mapTo(viewport, control->rect().topLeft()).x();
            CHECK(left >= 0);
            CHECK(left + control->width() <= viewport->width());
        }
    }
}

// from tests/test_guided_setup.py::test_short_pages_do_not_scroll_at_the_default_size
TEST_CASE("test_short_pages_do_not_scroll_at_the_default_size", "[test_guided_setup][widget]") {
    const int page =
        GENERATE(GuidedSetupDialog::Record, GuidedSetupDialog::Output, GuidedSetupDialog::Ready);
    CAPTURE(page);
    fvtest::AppTheme theme;
    SetupRig rig(Json{{"transcript_cleanup_enabled", true}});
    GuidedSetupDialog& setup = rig.setup();
    setup.show();
    setup.resize(720, 690);
    setup.showPage(page);
    QTest::qWait(30);
    CHECK(setup.scrollArea()->verticalScrollBar()->maximum() == 0);
}

TEST_CASE("rows stack below 620 px at 100 % and the window keeps its sizes",
          "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    CHECK(setup.minimumSize() == QSize(540, 460));
    CHECK(setup.size() == QSize(720, 690));
    CHECK(setup.isSizeGripEnabled());
    CHECK_FALSE(setup.isModal());
    CHECK(setup.objectName() == QStringLiteral("guidedSetupDialog"));
    CHECK(setup.windowTitle() == QStringLiteral("Set up ") + product());
    QWidget* holder = setup.speechSummary()->parentWidget();
    REQUIRE(holder != nullptr);
    setup.resize(619, 690);
    QWidget* trigger = setup.triggerCombo();
    // Stacked, the control sits under its text; side by side, beside it.
    setup.show();
    QTest::qWait(20);
    const QPoint stacked = trigger->mapTo(&setup, QPoint());
    setup.resize(620, 690);
    QTest::qWait(20);
    const QPoint side = trigger->mapTo(&setup, QPoint());
    CHECK(stacked.x() < side.x());
}

// from
// tests/test_guided_setup.py::test_stale_microphone_projection_does_not_reset_to_system_default
TEST_CASE("test_stale_microphone_projection_does_not_reset_to_system_default",
          "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    waitForDevices(*rig.settings());
    const Json unavailable = Json{{"name", "Saved microphone"}, {"hostapi", "MME"}};
    rig.store().save_setting("audio_input_priority", Json::array({unavailable}));
    {
        const QSignalBlocker blocker(setup.microphoneCombo());
        setup.microphoneCombo()->addItem(QStringLiteral("No longer in inventory"),
                                         QStringLiteral("stale-token"));
        setup.microphoneCombo()->setCurrentIndex(setup.microphoneCombo()->count() - 1);
    }
    Q_EMIT setup.microphoneCombo()->currentIndexChanged(0);
    CHECK(rig.saved("audio_input_priority") == Json::array({unavailable}));
}

// from
// tests/test_guided_setup.py::test_recording_shortcut_keyboard_capture_saves_and_keeps_record_step
// The field captures in Qt itself; the key events go to the widget, not to the desktop.
TEST_CASE("test_recording_shortcut_keyboard_capture_saves_and_keeps_record_step",
          "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    setup.show();
    setup.shortcutInput()->setFocus();
    setup.shortcutInput()->beginCapture();
    QTest::qWait(20);
    QTest::keyClick(setup.shortcutInput(), Qt::Key_J, Qt::ControlModifier | Qt::AltModifier);
    CHECK_FALSE(setup.shortcutInput()->isCapturing());
    CHECK(rig.hotkeys()["record_toggle"] == "ctrl+alt+j");
    CHECK(setup.page() == GuidedSetupDialog::Record);
}

// ---- C++ contracts ----

TEST_CASE("the Welcome page ends with an empty extras layout that adds no height",
          "[test_guided_setup][widget]") {
    SetupRig rig(Json::object(), true);
    GuidedSetupDialog& setup = rig.setup();
    QVBoxLayout* extras = setup.welcomeExtras();
    REQUIRE(extras != nullptr);
    CHECK(extras->count() == 0);
    CHECK(extras->spacing() == 0);
    CHECK(extras->contentsMargins() == QMargins());
    QLayout* body = setup.pages()->widget(GuidedSetupDialog::Welcome)->layout();
    REQUIRE(body != nullptr);
    CHECK(body->itemAt(body->count() - 1)->layout() == extras);
}

TEST_CASE("a custom prompt leaves no tile selected until a level wins again",
          "[test_guided_setup][widget]") {
    SetupRig rig(Json{{"transcript_cleanup_enabled", true},
                      {"transcript_cleanup_prompt", "Use my exact prompt"},
                      {"transcript_cleanup_level", "light"}});
    GuidedSetupDialog& setup = rig.setup();
    setup.showPage(GuidedSetupDialog::CleanUp);
    const auto anyChecked = [&] {
        for (QAbstractButton* tile : setup.levels()->buttons()) {
            if (tile->isChecked()) {
                return true;
            }
        }
        return false;
    };
    CHECK_FALSE(anyChecked());
    CHECK(setup.preview()->isHidden());
    CHECK_FALSE(setup.levelNote()->isHidden());
    // Declined: the prompt stays and no tile is selected.
    rig.confirmAnswer = false;
    setup.levels()->buttons()[0]->click();
    REQUIRE(fvtest::waitForQueuedRefresh(setup));
    REQUIRE(rig.confirmed.size() == 1);
    CHECK(rig.confirmed.front() == QStringLiteral("light"));
    CHECK(rig.saved("transcript_cleanup_prompt") == "Use my exact prompt");
    CHECK_FALSE(anyChecked());
    // Accepted: the prompt goes and the level's tile is selected, the one clicked before too.
    rig.confirmAnswer = true;
    setup.levels()->buttons()[0]->click();
    REQUIRE(fvtest::waitForQueuedRefresh(setup));
    CHECK_FALSE(rig.all().contains("transcript_cleanup_prompt"));
    CHECK(rig.saved("transcript_cleanup_level") == "light");
    CHECK(setup.levels()->buttons()[0]->isChecked());
    CHECK(setup.sampleResult()->text() == QStringLiteral("Meet at two, actually three."));
}

TEST_CASE("the cleanup switch saves through Settings and the provider row waits for M4",
          "[test_guided_setup][widget]") {
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    setup.showPage(GuidedSetupDialog::CleanUp);
    setup.cleanupSwitch()->click();
    CHECK(rig.saved("transcript_cleanup_enabled") == true);
    CHECK_FALSE(setup.cleanupOptions()->isHidden());
    CHECK(setup.privacyLabel()->isHidden());
    CHECK(setup.providerButton()->isHidden());
    CHECK(setup.sampleResult()->text() == QStringLiteral("Meet at 3."));
    setup.cleanupSwitch()->click();
    CHECK(rig.saved("transcript_cleanup_enabled") == false);
    CHECK(setup.cleanupOptions()->isHidden());
}
