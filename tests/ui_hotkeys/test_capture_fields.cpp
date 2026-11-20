// Ported from tests/test_personalize_s6_settings.py: the Hotkeys page's standard rows, the
// double-tap switch, side-button capture, Escape, the capture fields and the fit of the rows.
// The global capture is hotkeys::FakeCapture; nothing here hooks the keyboard or the mouse.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <tuple>
#include <vector>

#include <QAbstractButton>
#include <QComboBox>
#include <QCoreApplication>
#include <QFocusEvent>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTimer>

#include "hotkeys/mouse_shortcuts.h"
#include "hotkeys/testing/vocabulary.h"
#include "hotkeys_test_support.h"
#include "settings/keys.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/settings_switch.h"

using fv::Json;
using namespace fvtest;

namespace {

constexpr std::array<const char*, 4> kNewRows{"command_mode", "scratchpad_toggle", "cycle_language",
                                              "paste_last_original"};

void selectMode(fv::HotkeysPage& page, const char* mode) {
    auto* combo = page.recordModeCombo();
    combo->setCurrentIndex(combo->findData(QString::fromLatin1(mode)));
}

} // namespace

// from
// tests/test_personalize_s6_settings.py::test_new_actions_are_optional_rows_with_their_own_clear
TEST_CASE("new actions are optional rows with their own clear", "[test_personalize_s6_settings]") {
    PageScene scene(Json{{"hotkeys", {{"scratchpad_toggle", "ctrl+alt+n"}}}});
    auto& page = *scene.page;
    // OpenWhisper marks these fields with an `optional` property no theme rule reads; here an
    // optional row is the one with its own working Clear.
    for (const char* key : kNewRows) {
        INFO(key);
        REQUIRE(page.clearButton(key) != nullptr);
        CHECK_FALSE(page.hotkeyInput(key)->property("optional").isValid());
    }
    CHECK(page.hotkeyInput("scratchpad_toggle")->text() == display("ctrl+alt+n"));
    CHECK(page.clearButton("scratchpad_toggle")->isEnabled());
    CHECK_FALSE(page.clearButton("command_mode")->isEnabled());

    page.clearButton("scratchpad_toggle")->click();
    CHECK(scene.saved("scratchpad_toggle").empty());
    CHECK(scene.host->message() == QStringLiteral("Scratchpad hotkey cleared."));
    CHECK_FALSE(page.clearButton("scratchpad_toggle")->isEnabled());
}

// from
// tests/test_personalize_s6_settings.py::test_a_shortcut_another_action_uses_is_refused_on_its_row
TEST_CASE("a shortcut another action uses is refused on its row",
          "[test_personalize_s6_settings]") {
    PageScene scene;
    auto& page = *scene.page;
    page.show();
    const std::string cancel = page.currentHotkeys()["cancel"].get<std::string>();
    QTest::mouseClick(page.hotkeyInput("command_mode"), Qt::LeftButton);
    scene.capture()->capture(cancel);

    CHECK(scene.saved("command_mode").empty());
    CHECK(scene.note("command_mode") == "That shortcut is already used by Cancel.");
    CHECK(page.rowNote("command_mode")->property("tone").toString() == QStringLiteral("warning"));
    CHECK(scene.host->message() == QStringLiteral("That shortcut is already used by Cancel."));

    QTest::mouseClick(page.hotkeyInput("command_mode"), Qt::LeftButton);
    scene.capture()->capture("ctrl+alt+k");
    CHECK(scene.saved("command_mode") == "ctrl+alt+k");
    CHECK(scene.note("command_mode").empty());
    CHECK(scene.host->message() == QStringLiteral("Command Mode hotkey updated."));
}

// from
// tests/test_personalize_s6_settings.py::test_double_tap_switch_shows_for_push_and_hold_and_saves
TEST_CASE("double tap switch shows for push and hold and saves", "[test_personalize_s6_settings]") {
    PageScene scene;
    auto& page = *scene.page;
    page.show();
    QCoreApplication::processEvents();
    auto* toggle = page.handsFreeLatchSwitch();
    CHECK_FALSE(page.handsFreeLatchRow()->isVisible());
    CHECK(toggle->isChecked());

    selectMode(page, "push_hold");
    CHECK(page.handsFreeLatchRow()->isVisible());
    auto* description = page.rowDescription("record_toggle");
    CHECK(description->text().contains(QStringLiteral("Double-tap to record hands-free")));
    CHECK(scene.host->modes == std::vector<QString>{QStringLiteral("push_hold")});

    toggle->click();
    CHECK(scene.store->get(fv::keys::recording_hands_free_latch) == false);
    CHECK(description->text().contains(QStringLiteral("Quick taps are canceled")));

    selectMode(page, "toggle");
    CHECK_FALSE(page.handsFreeLatchRow()->isVisible());
}

// from tests/test_personalize_s6_settings.py::test_saved_latch_state_loads_with_the_page
TEST_CASE("saved latch state loads with the page", "[test_personalize_s6_settings]") {
    PageScene scene(
        Json{{"recording_trigger_mode", "push_hold"}, {"recording_hands_free_latch", false}});
    auto& page = *scene.page;
    CHECK_FALSE(page.handsFreeLatchSwitch()->isChecked());
    CHECK_FALSE(page.handsFreeLatchRow()->isHidden());
    CHECK(page.rowDescription("record_toggle")
              ->text()
              .contains(QStringLiteral("Quick taps are canceled")));
    // Loading saves nothing and tells the app nothing.
    CHECK(scene.host->modes.empty());
}

// from
// tests/test_personalize_s6_settings.py::test_side_button_click_on_the_capturing_field_saves_a_mouse_shortcut
TEST_CASE("side button click on the capturing field saves a mouse shortcut",
          "[test_personalize_s6_settings]") {
    // The Windows hook's vocabulary and wording, whatever this machine's backend.
    const fv::hotkeys::testing::ScopedVocabulary windows(fv::hotkeys::Vocabulary::Windows);
    PageScene scene;
    auto& page = *scene.page;
    auto* field = page.hotkeyInput("paste_last_original");
    page.show();
    QTest::mouseClick(field, Qt::LeftButton);
    CHECK(page.capturingKey() == "paste_last_original");
    CHECK(scene.host->suspends.back() == true);
    auto* capture = scene.capture();

    // The Windows wording.
    field->setMouseShortcutsSupported(true);
    clickSideButton(field, Qt::BackButton, Qt::ShiftModifier);

    CHECK(capture->stopped());
    CHECK_FALSE(page.capturingKey().has_value());
    CHECK(scene.host->suspends.back() == false);
    CHECK(scene.saved("paste_last_original") == "shift+mouse4");
    CHECK(field->text() == display("shift+mouse4"));
    CHECK(scene.note("paste_last_original") ==
          "Mouse 4 won't work as Back in other apps while it's a shortcut.");
}

// from tests/test_personalize_s6_settings.py::test_side_button_outside_capture_changes_nothing
TEST_CASE("side button outside capture changes nothing", "[test_personalize_s6_settings]") {
    PageScene scene;
    auto& page = *scene.page;
    page.show();
    clickSideButton(page.hotkeyInput("command_mode"), Qt::ForwardButton);
    CHECK_FALSE(page.capturingKey().has_value());
    CHECK(scene.captures.made.empty());
    CHECK(scene.saved("command_mode").empty());
}

// from tests/test_personalize_s6_settings.py::test_unsupported_desktop_explains_and_keeps_capturing
TEST_CASE("unsupported desktop explains and keeps capturing", "[test_personalize_s6_settings]") {
    PageScene scene;
    auto& page = *scene.page;
    page.show();
    auto* field = page.hotkeyInput("command_mode");
    QTest::mouseClick(field, Qt::LeftButton);
    field->setMouseShortcutsSupported(false);
    clickSideButton(field);
    CHECK(page.capturingKey() == "command_mode");
    CHECK(scene.note("command_mode") ==
          QString::fromUtf16(fv::kMouseUnsupportedMessage).toStdString());
    CHECK(scene.saved("command_mode").empty());
    page.cancelCapture();
    CHECK(scene.note("command_mode").empty());
}

// from
// tests/test_personalize_s6_settings.py::test_escape_cancels_capture_without_binding_a_global_shortcut
// The walkthrough rows are the Hotkeys page's (the Walkthroughs page's copy of them is M6's).
TEST_CASE("escape cancels capture without binding a global shortcut",
          "[test_personalize_s6_settings]") {
    const std::string key =
        GENERATE(as<std::string>{}, "command_mode", "record_toggle", "walkthrough", "screenshot");
    INFO(key);
    PageScene scene(Json{{"hotkeys", {{"walkthrough", "ctrl+alt+w"}}}});
    auto& page = *scene.page;
    page.show();
    auto* field = page.hotkeyInput(key);
    const std::string previous = page.currentHotkeys()[key].get<std::string>();
    QTest::mouseClick(field, Qt::LeftButton);
    CHECK(page.capturingKey() == key);
    CHECK(field->text() == QString::fromUtf16(fv::kCaptureHint));

    QTest::keyClick(field, Qt::Key_Escape);

    CHECK_FALSE(page.capturingKey().has_value());
    CHECK(page.currentHotkeys()[key] == previous);
    const Json saved = scene.savedHotkeys();
    CHECK(saved.value(key, previous) == previous);
    CHECK(saved.value(key, std::string()) != "esc");
    CHECK(field->text() == display(previous));
    CHECK(page.isVisible());
    CHECK_FALSE(scene.host->message().toLower().contains(QStringLiteral("updated")));
}

// from tests/test_personalize_s6_settings.py::test_captured_escape_from_the_listener_is_not_saved
TEST_CASE("captured escape from the listener is not saved", "[test_personalize_s6_settings]") {
    PageScene scene;
    auto& page = *scene.page;
    page.show();
    auto* field = page.hotkeyInput("screenshot");
    QTest::mouseClick(field, Qt::LeftButton);
    CHECK(page.capturingKey() == "screenshot");

    scene.capture()->capture("esc");

    CHECK_FALSE(page.capturingKey().has_value());
    CHECK(scene.saved("screenshot").empty());
    CHECK(field->text() == display(""));
}

// from tests/test_personalize_s6_settings.py::test_escape_with_modifiers_is_still_a_shortcut
TEST_CASE("escape with modifiers is still a shortcut", "[test_personalize_s6_settings]") {
    PageScene scene;
    auto& page = *scene.page;
    page.show();
    auto* field = page.hotkeyInput("screenshot");
    QTest::mouseClick(field, Qt::LeftButton);
    QTest::keyClick(field, Qt::Key_Escape, Qt::ControlModifier | Qt::AltModifier);
    CHECK(page.capturingKey() == "screenshot");

    scene.capture()->capture("ctrl+shift+escape");

    CHECK(scene.saved("screenshot") == "ctrl+shift+escape");
}

// from
// tests/test_personalize_s6_settings.py::test_capture_field_turns_side_buttons_into_shortcuts_only_while_capturing
TEST_CASE("capture field turns side buttons into shortcuts only while capturing",
          "[test_personalize_s6_settings]") {
    fv::HotkeyCaptureInput field;
    field.show();
    QSignalSpy requested(&field, &fv::HotkeyCaptureInput::captureRequested);
    QSignalSpy captured(&field, &fv::HotkeyCaptureInput::mouseCaptured);
    QSignalSpy notices(&field, &fv::HotkeyCaptureInput::notice);

    field.setMouseShortcutsSupported(true);
    clickSideButton(&field);
    CHECK(requested.count() == 0);
    CHECK(captured.count() == 0);
    field.setCapturing(true);
    clickSideButton(&field, Qt::ForwardButton, Qt::ControlModifier);
    QTest::mouseClick(&field, Qt::LeftButton);

    REQUIRE(captured.count() == 1);
    CHECK(captured.at(0).at(0).toString() == QStringLiteral("ctrl+mouse5"));
    CHECK(requested.count() == 1);
    CHECK(notices.count() == 0);
}

// from tests/test_personalize_s6_settings.py::test_profile_field_captures_a_side_button_and_stops
TEST_CASE("profile field captures a side button and stops", "[test_personalize_s6_settings]") {
    fv::ProfileHotkeyInput field;
    field.show();
    QSignalSpy captured(&field, &fv::ProfileHotkeyInput::captured);
    std::vector<bool> changes;
    QObject::connect(&field, &fv::ProfileHotkeyInput::captureChanged,
                     [&](bool capturing) { changes.push_back(capturing); });
    field.beginCapture();
    field.setMouseShortcutsSupported(true);
    clickSideButton(&field);
    REQUIRE(captured.count() == 1);
    CHECK(captured.at(0).at(0).toString() == QStringLiteral("mouse4"));
    CHECK(field.hotkey() == QStringLiteral("mouse4"));
    CHECK(changes == std::vector<bool>{true, false});
}

// from tests/test_personalize_s6_settings.py::test_side_button_notes_depend_on_the_desktop
TEST_CASE("side button notes depend on the desktop", "[test_personalize_s6_settings]") {
    using fv::hotkeys::mouse_shortcut_note;
    using fv::hotkeys::Vocabulary;
    CHECK(mouse_shortcut_note("mouse5", Vocabulary::Windows).find("won't work as Forward") !=
          std::string::npos);
    CHECK(mouse_shortcut_note("ctrl+r", Vocabulary::Windows).empty());
    CHECK(mouse_shortcut_note("mouse4", Vocabulary::Pynput) ==
          "Mouse 4 also still goes Back in the app you're using.");
    CHECK(mouse_shortcut_note("alt+mouse4", Vocabulary::Pynput).empty());
}

// from
// tests/test_personalize_s6_settings.py::test_escape_on_a_capturing_field_emits_cancelled_not_a_shortcut
TEST_CASE("escape on a capturing field emits cancelled not a shortcut",
          "[test_personalize_s6_settings]") {
    fv::HotkeyCaptureInput field;
    field.show();
    QSignalSpy cancelled(&field, &fv::HotkeyCaptureInput::captureCancelled);
    QSignalSpy requested(&field, &fv::HotkeyCaptureInput::captureRequested);
    field.setCapturing(true);
    QTest::keyClick(&field, Qt::Key_Escape);
    CHECK(cancelled.count() == 1);
    CHECK(requested.count() == 0);
    field.setCapturing(false);
    QTest::keyClick(&field, Qt::Key_Escape);
    CHECK(cancelled.count() == 1);
}

// The ProfileHotkeyInput key map (profile_hotkey_input.py keyPressEvent and keyReleaseEvent).
TEST_CASE("profile field commits on the release of its main key",
          "[test_personalize_s6_settings]") {
    fv::ProfileHotkeyInput field;
    field.show();
    QSignalSpy captured(&field, &fv::ProfileHotkeyInput::captured);

    SECTION("a letter with modifiers") {
        field.beginCapture();
        CHECK(field.text() == QString::fromUtf16(fv::kCaptureHint));
        QTest::keyPress(&field, Qt::Key_N, Qt::ControlModifier | Qt::AltModifier);
        CHECK(captured.count() == 0);
        QTest::keyRelease(&field, Qt::Key_N, Qt::ControlModifier | Qt::AltModifier);
        REQUIRE(captured.count() == 1);
        CHECK(captured.at(0).at(0).toString() == QStringLiteral("ctrl+alt+n"));
        CHECK_FALSE(field.isCapturing());
        CHECK(field.text() == display("ctrl+alt+n"));
    }
    SECTION("only the last key pressed counts") {
        field.beginCapture();
        QTest::keyPress(&field, Qt::Key_A);
        QTest::keyPress(&field, Qt::Key_F5);
        QTest::keyRelease(&field, Qt::Key_A);
        CHECK(captured.count() == 0);
        QTest::keyRelease(&field, Qt::Key_F5);
        REQUIRE(captured.count() == 1);
        CHECK(captured.at(0).at(0).toString() == QStringLiteral("f5"));
    }
    SECTION("keypad keys and Tab") {
        field.beginCapture();
        QTest::keyClick(&field, Qt::Key_Asterisk, Qt::KeypadModifier);
        REQUIRE(captured.count() == 1);
        CHECK(captured.at(0).at(0).toString() == QStringLiteral("kp *"));
        field.beginCapture();
        QTest::keyClick(&field, Qt::Key_Tab, Qt::ShiftModifier);
        REQUIRE(captured.count() == 2);
        CHECK(captured.at(1).at(0).toString() == QStringLiteral("shift+tab"));
    }
    SECTION("Escape with modifiers cancels") {
        field.setHotkey(QStringLiteral("f8"));
        field.beginCapture();
        QTest::keyClick(&field, Qt::Key_Escape, Qt::ControlModifier | Qt::AltModifier);
        CHECK_FALSE(field.isCapturing());
        CHECK(captured.count() == 0);
        CHECK(field.text() == display("f8"));
    }
    SECTION("ignored keys and Space or Enter start a capture") {
        QTest::keyClick(&field, Qt::Key_Space);
        CHECK(field.isCapturing());
        QTest::keyClick(&field, Qt::Key_CapsLock);
        QTest::keyClick(&field, Qt::Key_Plus);
        CHECK(captured.count() == 0);
        CHECK(field.isCapturing());
    }
}

TEST_CASE("profile field key names follow the platform", "[test_personalize_s6_settings]") {
    using fv::ProfileHotkeyInput;
    CHECK(ProfileHotkeyInput::keyName(Qt::Key_PageUp, "win32") == QStringLiteral("page up"));
    CHECK(ProfileHotkeyInput::keyName(Qt::Key_PageDown, "linux") == QStringLiteral("page_down"));
    CHECK(ProfileHotkeyInput::keyName(Qt::Key_F24, "win32") == QStringLiteral("f24"));
    CHECK(ProfileHotkeyInput::keyName(Qt::Key_Backtab, "win32") == QStringLiteral("tab"));
    CHECK(ProfileHotkeyInput::keyName(Qt::Key_Enter, "win32") == QStringLiteral("enter"));
    CHECK(ProfileHotkeyInput::keyName(Qt::Key_Exclam, "win32") == QStringLiteral("!"));
    CHECK(ProfileHotkeyInput::keyName(Qt::Key_Plus, "win32").isEmpty());
    CHECK(ProfileHotkeyInput::keyName(Qt::Key_Print, "win32").isEmpty());
}

TEST_CASE("profile field gives up after ten seconds", "[test_personalize_s6_settings]") {
    fv::ProfileHotkeyInput field;
    field.show();
    std::vector<bool> changes;
    QObject::connect(&field, &fv::ProfileHotkeyInput::captureChanged,
                     [&](bool capturing) { changes.push_back(capturing); });
    field.beginCapture();
    auto* timer = field.findChild<QTimer*>();
    REQUIRE(timer != nullptr);
    CHECK(timer->isSingleShot());
    CHECK(timer->interval() == 10000);
    CHECK(timer->isActive());
    QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
    CHECK_FALSE(field.isCapturing());
    CHECK(changes == std::vector<bool>{true, false});
}

// from tests/test_personalize_s6_settings.py::test_hotkey_rows_fit_narrow_windows_and_large_fonts
// The page on its own at 130 % fonts, in the widths Settings gives it in a narrow window.
TEST_CASE("hotkey rows fit narrow windows and large fonts", "[test_personalize_s6_settings]") {
    const int width = GENERATE(520, 400, 300);
    INFO("width " << width);
    // The Windows hook's vocabulary: its note on a side button with modifiers is the row note
    // checked below.
    const fv::hotkeys::testing::ScopedVocabulary windows(fv::hotkeys::Vocabulary::Windows);
    AppTheme theme(QStringLiteral("dark"), 130);
    PageScene scene(
        Json{{"recording_trigger_mode", "push_hold"},
             {"hotkeys", {{"command_mode", "ctrl+alt+shift+mouse4"}}},
             {"transcript_cleanup_profiles",
              {{{"id", "email"}, {"name", "Email"}, {"instructions", "An email."}}}},
             {"text_transforms",
              {{{"id", "polish"}, {"name", "Polish"}, {"instruction", "Polish it."}}}}});
    scene.addListSources();
    auto& page = *scene.page;
    page.resize(width, 2400);
    page.show();
    for (int i = 0; i < 10; ++i) {
        QCoreApplication::processEvents();
    }
    CHECK(page.handsFreeLatchRow()->isVisible());
    CHECK(page.rowNote("command_mode")->isVisible());
    std::vector<fv::ReflowCard*> cards;
    for (auto* card : page.findChildren<fv::ReflowCard*>()) {
        if (card->isVisible()) {
            cards.push_back(card);
        }
    }
    // Thirteen standard shortcuts and the record mode, then the profile and the transform.
    CHECK(cards.size() == 16);
    for (auto* card : cards) {
        std::vector<std::array<int, 4>> rects;
        std::vector<QWidget*> controls;
        for (auto* button : card->findChildren<QAbstractButton*>()) {
            controls.push_back(button);
        }
        for (auto* edit : card->findChildren<QLineEdit*>()) {
            controls.push_back(edit);
        }
        for (auto* control : controls) {
            if (!control->isVisible()) {
                continue;
            }
            const QPoint start = control->mapTo(card, control->rect().topLeft());
            const QPoint end = control->mapTo(card, control->rect().bottomRight());
            rects.push_back({start.x(), start.y(), end.x(), end.y()});
        }
        std::sort(rects.begin(), rects.end());
        for (const auto& [left, top, right, bottom] : rects) {
            CHECK(left >= 0);
            CHECK(right < card->width());
        }
        for (std::size_t i = 0; i + 1 < rects.size(); ++i) {
            const auto& first = rects[i];
            const auto& second = rects[i + 1];
            if (first[1] <= second[3] && second[1] <= first[3]) {
                CHECK(first[2] < second[0]);
            }
        }
    }
    // assert_wrapped_labels_fit: no visible word-wrapped label is cut off.
    for (auto* label : page.findChildren<QLabel*>()) {
        if (label->isVisible() && label->wordWrap()) {
            INFO(label->text().toStdString());
            CHECK(label->height() >= label->heightForWidth(label->width()));
        }
    }
}

// docs/port/map/a06-hotkeys-paste-focus.md A06-47: what every shortcut field is before anything is
// pressed (hotkey_capture.py HotkeyCaptureInput.__init__ and set_capturing).
TEST_CASE("a shortcut field is a read-only line that marks itself while capturing",
          "[capture_field]") {
    fv::HotkeyCaptureInput field;
    CHECK(field.objectName() == QStringLiteral("hotkeyInput"));
    CHECK(field.isReadOnly());
    CHECK(field.minimumHeight() == 38);
    CHECK(field.placeholderText() == QStringLiteral("Click to set hotkey"));
    CHECK_FALSE(field.property("capturing").toBool());

    field.setCapturing(true);
    CHECK(field.property("capturing").toBool());
    field.setCapturing(false);
    CHECK_FALSE(field.property("capturing").toBool());
}

// A06-49: profile_hotkey_input.py's accessible name and tooltip, and focusOutEvent and hideEvent
// cancelling a capture.
TEST_CASE("profile field names itself and a capture ends when it loses focus or is hidden",
          "[capture_field]") {
    fv::ProfileHotkeyInput field;
    CHECK(field.accessibleName() == QStringLiteral("Profile recording shortcut"));
    CHECK(field.toolTip() ==
          QStringLiteral(
              "Click, then press a shortcut. Escape cancels. Profile shortcuts toggle recording."));
    field.setHotkey(QStringLiteral("f8"));
    field.show();
    QSignalSpy captured(&field, &fv::ProfileHotkeyInput::captured);
    std::vector<bool> changes;
    QObject::connect(&field, &fv::ProfileHotkeyInput::captureChanged,
                     [&](bool capturing) { changes.push_back(capturing); });

    field.beginCapture();
    REQUIRE(field.isCapturing());
    QFocusEvent out(QEvent::FocusOut);
    QCoreApplication::sendEvent(&field, &out);
    CHECK_FALSE(field.isCapturing());
    CHECK(field.text() == display("f8"));

    field.beginCapture();
    REQUIRE(field.isCapturing());
    field.hide();
    CHECK_FALSE(field.isCapturing());

    CHECK(changes == std::vector<bool>{true, false, true, false});
    CHECK(captured.count() == 0);
    CHECK(field.hotkey() == QStringLiteral("f8"));
}
