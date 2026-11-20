// Ported from tests/test_hotkey_settings_page.py, the Hotkeys helpers of
// tests/test_personalize_foundation_ui_settings.py and the shortcut-name tests of
// tests/test_automation_fix_pipeline_sources.py. The page runs over a settings file in a
// temporary folder, a stand-in host and fake global captures.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <set>
#include <string>
#include <vector>

#include <QComboBox>

#include "hotkeys/conflicts.h"
#include "hotkeys/defaults.h"
#include "hotkeys_test_support.h"
#include "settings/keys.h"
#include "ui_hotkeys/standard_hotkeys.h"
#include "ui_widgets/buttons.h"

using fv::Json;
using namespace fvtest;

namespace {

Json defaults() {
    Json out = Json::object();
    for (const auto& [action, hotkey] : fv::hotkeys::default_hotkeys()) {
        out[action] = hotkey;
    }
    return out;
}

} // namespace

// from
// tests/test_hotkey_settings_page.py::TestHotkeySettingsPage::test_partial_settings_merge_with_platform_defaults
TEST_CASE("partial settings merge with platform defaults", "[test_hotkey_settings_page]") {
    PageScene scene(Json{{"hotkeys", {{"record_toggle", "f8"}}}});
    auto& page = *scene.page;
    CHECK(page.currentHotkeys()["record_toggle"] == "f8");
    CHECK(page.currentHotkeys()["cancel"] == defaults()["cancel"]);
    CHECK(page.hotkeyInput("record_toggle")->text() == display("f8"));
    CHECK(fv::HotkeysPage::hotkeysRailValue(*scene.store->snapshot()) == display("f8"));
}

// _load_hotkey_settings: DEFAULT_HOTKEYS.update(saved or {}); a saved value update() cannot take
// leaves the defaults (logged), an empty one is the defaults.
TEST_CASE("unreadable saved hotkeys load as the platform defaults", "[test_hotkey_settings_page]") {
    const Json saved = GENERATE(Json::array({1}), Json("f8x"), Json(5), Json::object(),
                                Json(nullptr), Json::array());
    INFO(saved.dump());
    PageScene scene(Json{{"hotkeys", saved}});
    CHECK(scene.page->currentHotkeys() == defaults());
    CHECK(scene.page->hotkeyInput("record_toggle")->text() == display("kp *"));
    CHECK(scene.host->applied.empty());
}

TEST_CASE("the rail value is the record shortcut or Not set", "[test_hotkey_settings_page]") {
    CHECK(fv::HotkeysPage::hotkeysRailValue(Json::object()) == display("kp *"));
    CHECK(fv::HotkeysPage::hotkeysRailValue(Json{{"hotkeys", {{"cancel", "f9"}}}}) ==
          QStringLiteral("Not set"));
    CHECK(fv::HotkeysPage::hotkeysRailValue(Json{{"hotkeys", {{"record_toggle", ""}}}}) ==
          QStringLiteral("Not set"));
}

// from
// tests/test_hotkey_settings_page.py::TestHotkeySettingsPage::test_capture_applies_and_persists_immediately
TEST_CASE("capture applies and persists immediately", "[test_hotkey_settings_page]") {
    PageScene scene;
    auto& page = *scene.page;
    page.show();
    auto* field = page.hotkeyInput("record_toggle");
    QTest::mouseClick(field, Qt::LeftButton);
    REQUIRE(scene.capture() != nullptr);
    CHECK(scene.capture()->starts() == 1);

    scene.capture()->capture("ctrl+shift+r");

    REQUIRE_FALSE(scene.host->applied.empty());
    CHECK(scene.host->applied.back()["record_toggle"] == "ctrl+shift+r");
    CHECK(scene.saved("record_toggle") == "ctrl+shift+r");
    CHECK(field->text() == display("ctrl+shift+r"));
    CHECK(scene.host->message().toLower().contains(QStringLiteral("updated")));
}

// from
// tests/test_hotkey_settings_page.py::TestHotkeySettingsPage::test_clear_and_confirmed_reset_apply_immediately
TEST_CASE("clear and confirmed reset apply immediately", "[test_hotkey_settings_page]") {
    Json custom = defaults();
    custom["meeting_toggle"] = "ctrl+alt+m";
    custom["record_toggle"] = "f9";
    PageScene scene(Json{{"hotkeys", custom}});
    auto& page = *scene.page;

    page.clearButton("meeting_toggle")->click();
    REQUIRE(scene.host->applied.size() == 1);
    CHECK(scene.host->applied.back()["meeting_toggle"] == "");

    scene.resetAnswer = false;
    page.resetButton()->click();
    CHECK(scene.host->applied.size() == 1);

    scene.resetAnswer = true;
    page.resetButton()->click();
    REQUIRE(scene.host->applied.size() == 2);
    CHECK(scene.host->applied.back() == defaults());
    CHECK(scene.host->message() == QStringLiteral("Hotkeys reset to platform defaults."));
}

// from
// tests/test_hotkey_settings_page.py::TestHotkeySettingsPage::test_recording_trigger_mode_loads_and_applies
TEST_CASE("recording trigger mode loads and applies", "[test_hotkey_settings_page]") {
    PageScene scene(Json{{"recording_trigger_mode", "push_hold"}});
    auto& page = *scene.page;
    CHECK(page.recordModeCombo()->currentData().toString() == QStringLiteral("push_hold"));
    CHECK(page.rowDescription("record_toggle")->text().contains(QStringLiteral("Hold to record")));

    auto* combo = page.recordModeCombo();
    combo->setCurrentIndex(combo->findData(QStringLiteral("toggle")));

    CHECK(scene.host->modes == std::vector<QString>{QStringLiteral("toggle")});
    CHECK(scene.store->get(fv::keys::recording_trigger_mode) == "toggle");
    CHECK(page.rowDescription("record_toggle")
              ->text()
              .contains(QStringLiteral("Start recording when idle")));
}

// from
// tests/test_hotkey_settings_page.py::TestHotkeySettingsPage::test_leaving_hotkeys_stops_active_capture
// Settings calls the page's cancelCapture hook when it switches to another page.
TEST_CASE("leaving hotkeys stops active capture", "[test_hotkey_settings_page]") {
    PageScene scene;
    auto& page = *scene.page;
    page.show();
    auto* field = page.hotkeyInput("record_toggle");
    QTest::mouseClick(field, Qt::LeftButton);
    auto* capture = scene.capture();
    REQUIRE(capture != nullptr);
    CHECK(field->property("capturing").toBool());
    CHECK(capture->captured.subscriber_count() == 1);

    static_cast<fv::SettingsPageModule&>(page).cancelCapture();

    CHECK(capture->captured.subscriber_count() == 0);
    CHECK(capture->cancelled.subscriber_count() == 0);
    CHECK(capture->failed.subscriber_count() == 0);
    CHECK(capture->stopped());
    CHECK_FALSE(page.capturingKey().has_value());
    CHECK_FALSE(field->property("capturing").toBool());
    // A result the stopped capture still delivers changes nothing.
    capture->capture("ctrl+shift+q");
    CHECK(scene.host->applied.empty());
    CHECK_FALSE(scene.store->snapshot()->contains("hotkeys"));
    CHECK(page.currentHotkeys()["record_toggle"] == "kp *");
}

TEST_CASE("a result from an earlier capture is ignored", "[test_hotkey_settings_page]") {
    PageScene scene;
    auto& page = *scene.page;
    page.show();
    QTest::mouseClick(page.hotkeyInput("command_mode"), Qt::LeftButton);
    auto* first = scene.capture();
    QTest::mouseClick(page.hotkeyInput("scratchpad_toggle"), Qt::LeftButton);
    auto* second = scene.capture();
    REQUIRE(first != second);
    CHECK(first->stopped());
    CHECK(page.capturingKey() == "scratchpad_toggle");

    // The first capture's finish arrives late; only the running one's result counts.
    CHECK(first->finish_stopped());
    second->capture("ctrl+alt+n");
    CHECK(scene.saved("command_mode").empty());
    CHECK(scene.saved("scratchpad_toggle") == "ctrl+alt+n");
    QCoreApplication::processEvents();
    CHECK(scene.host->suspends == std::vector<bool>{true, false, true, false});
}

TEST_CASE("a failed capture says so and keeps the shortcut", "[test_hotkey_settings_page]") {
    PageScene scene;
    auto& page = *scene.page;
    page.show();
    auto* field = page.hotkeyInput("cancel");
    QTest::mouseClick(field, Qt::LeftButton);
    scene.capture()->fail();
    REQUIRE(scene.failures.size() == 1);
    CHECK(scene.failures[0] == QStringLiteral("Could not capture hotkey. Please try again."));
    CHECK_FALSE(page.capturingKey().has_value());
    CHECK(field->text() == display("kp -"));
    CHECK(scene.host->applied.empty());
    CHECK(scene.host->suspends.back() == false);
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestDialogHelpers::test_standard_hotkey_survives_a_later_hotkeys_page_edit
TEST_CASE("standard hotkey survives a later hotkeys page edit",
          "[test_personalize_foundation_ui_settings]") {
    PageScene scene;
    auto& page = *scene.page;
    CHECK(page.setStandardHotkey("command_mode", "ctrl+alt+k").isEmpty());
    CHECK(scene.saved("command_mode") == "ctrl+alt+k");
    CHECK(scene.host->message() == QStringLiteral("Command Mode hotkey updated."));

    page.loadSettings(*scene.store->snapshot());
    page.show();
    QTest::mouseClick(page.hotkeyInput("cancel"), Qt::LeftButton);
    scene.capture()->capture("ctrl+shift+x");
    CHECK(scene.saved("cancel") == "ctrl+shift+x");
    CHECK(scene.saved("command_mode") == "ctrl+alt+k");
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestDialogHelpers::test_standard_hotkey_conflicts_match_any_spelling_and_profiles
TEST_CASE("standard hotkey conflicts match any spelling and profiles",
          "[test_personalize_foundation_ui_settings]") {
    PageScene scene(Json{{"transcript_cleanup_profiles",
                          {{{"id", "ticket"},
                            {"name", "Ticket"},
                            {"instructions", "Format a ticket."},
                            {"hotkey", "ctrl+alt+t"}}}}});
    scene.addListSources();
    auto& page = *scene.page;
    CHECK(page.setStandardHotkey("command_mode", "ctrl+alt+k").isEmpty());
    const QString error = page.setStandardHotkey("scratchpad_toggle", "Alt+Ctrl+K");
    CHECK(error == QStringLiteral("That shortcut is already used by Command Mode."));
    CHECK(scene.host->message() == error);
    CHECK(page.setStandardHotkey("scratchpad_toggle", "alt+ctrl+t") ==
          QStringLiteral("That shortcut is already used by Ticket."));
    CHECK(scene.saved("scratchpad_toggle").empty());

    CHECK(page.setStandardHotkey("command_mode", "ctrl+alt+k").isEmpty());
    const std::string cancel = page.currentHotkeys()["cancel"].get<std::string>();
    CHECK(page.setStandardHotkey("cancel", cancel).isEmpty());
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestDialogHelpers::test_standard_hotkey_goes_through_the_app_registration
TEST_CASE("standard hotkey goes through the app registration",
          "[test_personalize_foundation_ui_settings]") {
    PageScene scene;
    auto& page = *scene.page;
    CHECK(page.setStandardHotkey("scratchpad_toggle", "ctrl+alt+n").isEmpty());
    REQUIRE(scene.host->applied.size() == 1);
    const Json& registered = scene.host->applied.back();
    CHECK(registered["scratchpad_toggle"] == "ctrl+alt+n");
    CHECK(registered["record_toggle"] == page.currentHotkeys()["record_toggle"]);
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestDialogHelpers::test_standard_hotkey_reports_why_it_was_not_saved
TEST_CASE("standard hotkey reports why it was not saved",
          "[test_personalize_foundation_ui_settings]") {
    PageScene scene;
    auto& page = *scene.page;
    const QString record =
        QString::fromStdString(page.currentHotkeys()["record_toggle"].get<std::string>());
    QString error = page.setStandardHotkey("command_mode", record.toUpper().toStdString());
    CHECK(error.contains(QStringLiteral("Recording")));
    CHECK(scene.saved("command_mode").empty());

    scene.host->saveError = "Disk full";
    error = page.setStandardHotkey("command_mode", "ctrl+alt+k");
    CHECK(error == QStringLiteral("Couldn't save hotkeys: Disk full"));
    CHECK(scene.host->message() == error);
    CHECK(page.currentHotkeys().value("command_mode", std::string()).empty());
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestDialogHelpers::test_clearing_a_standard_hotkey_never_conflicts
TEST_CASE("clearing a standard hotkey never conflicts",
          "[test_personalize_foundation_ui_settings]") {
    PageScene scene;
    CHECK(scene.page->setStandardHotkey("cycle_language", "").isEmpty());
    const Json saved = scene.savedHotkeys();
    REQUIRE(saved.contains("cycle_language"));
    CHECK(saved["cycle_language"] == "");
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestDialogHelpers::test_capture_suspension_uses_the_profile_capture_path
TEST_CASE("capture suspension uses the profile capture path",
          "[test_personalize_foundation_ui_settings]") {
    PageScene scene;
    scene.page->setHotkeyCaptureSuspended(true);
    scene.page->setHotkeyCaptureSuspended(false);
    CHECK(scene.host->suspends == std::vector<bool>{true, false});
}

TEST_CASE("a profile whose shortcut a standard change would take refuses the save",
          "[test_personalize_foundation_ui_settings]") {
    PageScene scene(Json{{"transcript_cleanup_profiles",
                          {{{"id", "ticket"},
                            {"name", "Ticket"},
                            {"instructions", "Format a ticket."},
                            {"hotkey", "ctrl+alt+t"}}}}});
    scene.addListSources();
    auto& page = *scene.page;
    page.show();
    // A captured shortcut a profile uses is refused on its row.
    QTest::mouseClick(page.hotkeyInput("scratchpad_toggle"), Qt::LeftButton);
    scene.capture()->capture("ctrl+alt+t");
    CHECK(scene.host->message() == QStringLiteral("That shortcut is already used by Ticket."));
    CHECK(scene.host->applied.empty());

    // The reset path checks every profile against the defaults.
    Json hotkeys = defaults();
    hotkeys["minimize_tray"] = "f7";
    scene.store->save_setting("hotkeys", hotkeys);
    Json profiles = Json::array(
        {{{"id", "ticket"}, {"name", "Ticket"}, {"instructions", "T."}, {"hotkey", "ctrl+alt+m"}}});
    scene.store->save_setting("transcript_cleanup_profiles", profiles);
    page.loadSettings(*scene.store->snapshot());
    page.resetButton()->click();
    CHECK(scene.host->message() ==
          QStringLiteral("Couldn't save hotkeys: Ticket's shortcut is already used by Minimize "
                         "to tray. Choose another."));
    CHECK(scene.host->applied.empty());
}

// from
// tests/test_automation_fix_pipeline_sources.py::test_conflicts_name_standard_shortcuts_as_people_read_them
TEST_CASE("conflicts name standard shortcuts as people read them",
          "[test_automation_fix_pipeline_sources]") {
    const Json settings{{"hotkeys", {{"minimize_tray", "ctrl+alt+m"}}}};
    const std::string record = defaults()["record_toggle"].get<std::string>();
    CHECK(fv::hotkeys::hotkey_conflict(record, settings) == "Recording");
    CHECK(fv::hotkeys::hotkey_conflict("ctrl+alt+m", settings) == "Minimize to tray");
    std::set<std::string> labelled;
    for (const auto& [action, label] : fv::hotkeys::standard_labels()) {
        labelled.insert(std::string(action));
    }
    for (const auto& [action, hotkey] : fv::hotkeys::default_hotkeys()) {
        INFO(action << " has no label");
        CHECK(labelled.contains(action));
    }
    fv::hotkeys::ConflictOptions options;
    options.exclude = {"standard", "record_toggle"};
    CHECK(fv::hotkeys::hotkey_conflict(record, settings, options).empty());
}

// from tests/test_automation_fix_pipeline_sources.py::test_the_settings_window_uses_the_same_labels
// The page names each standard shortcut in its messages by hotkeys::standard_labels().
TEST_CASE("the settings window uses the same labels", "[test_automation_fix_pipeline_sources]") {
    PageScene scene;
    for (const auto& [action, label] : fv::hotkeys::standard_labels()) {
        INFO(action);
        CHECK(fv::standard_hotkeys::label(action) == label);
        CHECK(scene.page->setStandardHotkey(std::string(action), "").isEmpty());
        CHECK(scene.host->message() ==
              QString::fromStdString(std::string(label) + " hotkey updated."));
    }
    CHECK(fv::standard_hotkeys::label("not_an_action") == "Shortcut");
}

TEST_CASE("the page names the controls deep links reach", "[test_hotkey_settings_page]") {
    PageScene scene;
    auto& module = static_cast<fv::SettingsPageModule&>(*scene.page);
    CHECK(module.namedControl("record_mode_combo") == scene.page->recordModeCombo());
    REQUIRE(module.namedControl("_hotkey_instruction") != nullptr);
    CHECK(module.namedControl("_hotkey_instruction")->objectName() ==
          QStringLiteral("hotkeyInstructionText"));
    CHECK(module.namedControl("nothing") == nullptr);
}
