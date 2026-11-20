// Quick Record's cleanup profile card (tests/test_cleanup_profiles.py, the three tests of the
// tab) and the behaviours the tab's source pins: the card shows only while AI cleanup is checked,
// a chosen profile locks the checkbox on, "Standard dictation" has data "", and the profile of
// the recording in progress stays listed.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>

#include "hotkeys/grammar.h"
#include "record_fixture.h"
#include "text/cleanup_profiles.h"
#include "ui_record/cleanup_profile_card.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::Json;

// The library the tests save: Email with a shortcut, and Support ticket without.
Json library() {
    return Json::array(
        {Json{{"id", "support-ticket"},
              {"name", "Support ticket"},
              {"instructions", "Format the dictation as a support ticket."},
              {"hotkey", ""}},
         Json{{"id", "email"},
              {"name", "Email"},
              {"instructions", "Turn the dictation into a concise, professional email."},
              {"hotkey", "ctrl+alt+e"}}});
}

void chooseProfile(fv::QuickRecordTab& tab, const char* id) {
    tab.profileCombo()->setCurrentIndex(tab.profileCombo()->findData(QString::fromLatin1(id)));
}

// The shortcut the button's hover pill shows, "" when it shows none.
QString hotkeyOf(fv::Button* button) {
    fv::HotkeyHintFilter* filter = button->hotkeyHintFilter();
    filter->showHint();
    if (filter->hint() == nullptr) {
        return {};
    }
    const QString shown = filter->hint()->findChild<QLabel*>()->text();
    filter->hideHint();
    return shown;
}

QString shortcut(const std::string& hotkey) {
    return QString::fromStdString(
        fv::hotkeys::format_hotkey_display(hotkey, fv::hotkeys::platform_vocabulary()));
}

} // namespace

// from tests/test_cleanup_profiles.py::
//      test_quick_record_selection_persists_without_changing_standard_cleanup
TEST_CASE("a quick record selection persists without changing standard cleanup",
          "[test_cleanup_profiles]") {
    fvtest::ShownTab first(Json{{"transcript_cleanup_enabled", false}});
    CHECK(first->selectedCleanupProfileId().isEmpty());
    chooseProfile(*first, "email");
    CHECK(first->cleanupCheck()->isChecked());
    CHECK_FALSE(first->cleanupCheck()->isEnabled());
    CHECK(first.settings.contents().at("transcript_cleanup_enabled") == false);
    CHECK(first.settings.contents().at("quick_record_profile") == "email");

    // A second tab over the same settings starts on the saved choice.
    fv::QuickRecordTab second(first.settings, fv::speechEngineCatalog());
    CHECK(second.selectedCleanupProfileId() == QStringLiteral("email"));
    second.setRecordingActive(true);
    CHECK_FALSE(second.profileCombo()->isEnabled());
    second.setRecordingActive(false);
    CHECK(second.profileCombo()->isEnabled());
    second.profileCombo()->setCurrentIndex(0);
    CHECK(second.cleanupCheck()->isEnabled());
    CHECK_FALSE(second.cleanupCheck()->isChecked());
}

// from tests/test_cleanup_profiles.py::
//      test_deleting_selected_profile_refreshes_quick_record_to_standard
TEST_CASE("deleting the selected profile refreshes quick record to standard",
          "[test_cleanup_profiles]") {
    fvtest::ShownTab tab(Json{{"transcript_cleanup_enabled", true}});
    chooseProfile(*tab, "email");
    tab.settings.mutate(
        [](Json& settings) { fv::text::delete_cleanup_profile(settings, "email"); });
    tab->refreshCleanupProfiles();
    CHECK(tab->selectedCleanupProfileId().isEmpty());
}

// from tests/test_cleanup_profiles.py::
//      test_hotkey_profile_is_shown_during_capture_without_changing_next_selection
TEST_CASE("a hotkey profile is shown during capture without changing the next selection",
          "[test_cleanup_profiles]") {
    fvtest::ShownTab tab(Json{{"transcript_cleanup_enabled", true}});
    tab->setRecordingProfile(QStringLiteral("email"));
    tab->setRecordingActive(true);
    CHECK(tab->selectedCleanupProfileId() == QStringLiteral("email"));
    CHECK(tab->profileHint()->text() == QStringLiteral("Recording with Email"));

    // Deleted while the recording runs: it stays listed and chosen.
    tab.settings.mutate(
        [](Json& settings) { fv::text::delete_cleanup_profile(settings, "email"); });
    tab->refreshCleanupProfiles();
    CHECK(tab->selectedCleanupProfileId() == QStringLiteral("email"));

    tab->setRecordingActive(false);
    CHECK(tab->selectedCleanupProfileId().isEmpty());
}

TEST_CASE("the card lists standard dictation first and shows only while cleanup is checked",
          "[widget]") {
    fvtest::ShownTab tab(
        Json{{"transcript_cleanup_enabled", false}, {"transcript_cleanup_profiles", library()}});
    CHECK_FALSE(tab->profileCard()->isVisible());

    tab->cleanupCheck()->setChecked(true);
    CHECK(tab->profileCard()->isVisible());
    QComboBox* combo = tab->profileCombo();
    REQUIRE(combo->count() == 3);
    CHECK(combo->itemText(0) == QStringLiteral("Standard dictation"));
    CHECK(combo->itemData(0).toString().isEmpty());
    CHECK(combo->itemText(1) == QStringLiteral("Support ticket"));
    CHECK(combo->itemData(1).toString() == QStringLiteral("support-ticket"));
    CHECK(combo->itemText(2) == QStringLiteral("Email"));
    CHECK(combo->accessibleName() == QStringLiteral("Quick Record cleanup profile"));
    CHECK(tab->profileCard()->manageButton()->text() == QStringLiteral(u"Manage…"));
    CHECK(tab->profileCard()->manageButton()->toolTip() ==
          QStringLiteral("Create cleanup profiles and assign recording shortcuts"));

    tab->cleanupCheck()->setChecked(false);
    CHECK_FALSE(tab->profileCard()->isVisible());
}

TEST_CASE("the hint says what the choice does", "[widget]") {
    fvtest::ShownTab tab(
        Json{{"transcript_cleanup_enabled", true}, {"transcript_cleanup_profiles", library()}});
    tab->updateHotkeys(QStringLiteral("Ctrl+Alt+R"), QStringLiteral("Esc"));
    CHECK(tab->profileHint()->text() ==
          QStringLiteral("Uses your usual cleanup settings and standard recording shortcut."));
    CHECK(tab->profileCombo()->toolTip() == QStringLiteral("Standard dictation"));
    CHECK(hotkeyOf(tab->recordButton()) == QStringLiteral("Ctrl+Alt+R"));

    chooseProfile(*tab, "email");
    CHECK(tab->profileHint()->text() ==
          QStringLiteral(u"Always uses AI cleanup · Record with %1").arg(shortcut("ctrl+alt+e")));
    CHECK(tab->profileCombo()->toolTip() ==
          QStringLiteral("Turn the dictation into a concise, professional email."));
    // The buttons show the profile's shortcut in place of the standard one.
    CHECK(hotkeyOf(tab->recordButton()) == shortcut("ctrl+alt+e"));
    CHECK(hotkeyOf(tab->stopButton()) == shortcut("ctrl+alt+e"));

    chooseProfile(*tab, "support-ticket");
    CHECK(tab->profileHint()->text() ==
          QStringLiteral(u"Always uses AI cleanup · Click Start Recording"));
    CHECK(hotkeyOf(tab->recordButton()).isEmpty());

    tab->profileCombo()->setCurrentIndex(0);
    CHECK(hotkeyOf(tab->recordButton()) == QStringLiteral("Ctrl+Alt+R"));
}

TEST_CASE("a chosen profile persists and locks the checkbox on; standard dictation frees it",
          "[widget]") {
    fvtest::ShownTab tab(
        Json{{"transcript_cleanup_enabled", true}, {"transcript_cleanup_profiles", library()}});
    chooseProfile(*tab, "support-ticket");
    CHECK(tab.settings.contents().at("quick_record_profile") == "support-ticket");
    CHECK_FALSE(tab->cleanupCheck()->isEnabled());

    tab->profileCombo()->setCurrentIndex(0);
    CHECK(tab.settings.contents().at("quick_record_profile") == "");
    CHECK(tab->cleanupCheck()->isEnabled());
    CHECK(tab->cleanupCheck()->isChecked());
}

TEST_CASE("a recording without a profile reads Standard dictation and locks the card", "[widget]") {
    fvtest::ShownTab tab(
        Json{{"transcript_cleanup_enabled", true}, {"transcript_cleanup_profiles", library()}});
    chooseProfile(*tab, "email");
    tab->setRecordingActive(true);
    // The saved choice is the next recording's; this one runs standard dictation.
    CHECK(tab->selectedCleanupProfileId().isEmpty());
    CHECK(tab->profileHint()->text() == QStringLiteral("Recording with Standard dictation"));
    CHECK_FALSE(tab->profileCombo()->isEnabled());

    tab->setRecordingActive(false);
    CHECK(tab->selectedCleanupProfileId() == QStringLiteral("email"));
    CHECK(tab->profileCombo()->isEnabled());
}

TEST_CASE("a recording profile that no longer exists is shown by its name", "[widget]") {
    fvtest::ShownTab tab(
        Json{{"transcript_cleanup_enabled", true}, {"transcript_cleanup_profiles", library()}});
    fv::text::CleanupProfile gone;
    gone.id = "gone";
    gone.name = "Gone";
    gone.instructions = "Do something.";
    tab->setRecordingProfile(std::optional<fv::text::CleanupProfile>(gone));
    tab->setRecordingActive(true);
    CHECK(tab->selectedCleanupProfileId() == QStringLiteral("gone"));
    CHECK(tab->profileHint()->text() == QStringLiteral("Recording with Gone"));
    // Listed after the saved ones, once.
    CHECK(tab->profileCombo()->count() == 4);
    CHECK(tab->profileCombo()->itemText(3) == QStringLiteral("Gone"));

    tab->setRecordingProfile(QStringLiteral("nothing-by-this-id"));
    tab->setRecordingActive(true);
    CHECK(tab->selectedCleanupProfileId().isEmpty());
}

TEST_CASE("Manage asks for the profile manager and a profile library is read from settings",
          "[widget]") {
    fvtest::ShownTab tab(Json{{"transcript_cleanup_enabled", true}});
    QSignalSpy requested(tab.tab.get(), &fv::QuickRecordTab::profilesRequested);
    tab->profileCard()->manageButton()->click();
    CHECK(requested.count() == 1);

    // No library saved: the two starters.
    CHECK(tab->profileCombo()->count() == 3);
    CHECK(tab->profileCombo()->itemData(2).toString() == QStringLiteral("email"));

    // A saved library replaces them, and a refresh shows it.
    tab.settings.save_setting("transcript_cleanup_profiles", library());
    tab->refreshCleanupProfiles();
    CHECK(tab->profileCombo()->count() == 3);
    CHECK(tab->profileCombo()->itemData(1).toString() == QStringLiteral("support-ticket"));
    tab.settings.save_setting("transcript_cleanup_profiles", Json::array());
    tab->refreshCleanupProfiles();
    CHECK(tab->profileCombo()->count() == 1);
}

TEST_CASE("a programmatic refresh does not save the selection", "[widget]") {
    fvtest::ShownTab tab(
        Json{{"transcript_cleanup_enabled", true}, {"quick_record_profile", "email"}});
    CHECK(tab->selectedCleanupProfileId() == QStringLiteral("email"));
    // Rebuilding the list moves the combo through its first item; nothing may be saved by it.
    tab->refreshCleanupProfiles();
    tab->setRecordingActive(true);
    tab->setRecordingActive(false);
    CHECK(tab.settings.contents().at("quick_record_profile") == "email");
}
