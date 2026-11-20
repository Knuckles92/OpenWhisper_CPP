// Settings > Personalize > Profiles: the cleanup profile library and editor
// (tests/test_cleanup_profiles.py, the editor's tests), over an in-memory settings file, and the
// page around it inside a real Settings window. QMessageBox.question is the panel's `question`
// seam.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QBoxLayout>
#include <QCheckBox>
#include <QCoreApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QTest>
#include <QTextEdit>

#include "settings/hotkeys.h"
#include "settings/testing/memory_store.h"
#include "styles_commands_support.h"
#include "text/cleanup_profiles.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_personalize/cleanup_profiles_panel.h"
#include "ui_personalize/profiles_page.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using fv::text::CleanupProfile;
using namespace fvtest;

namespace {

// The panel over a settings file of its own (OpenWhisper's isolated settings_manager), its
// questions answered `answer` and counted.
struct PanelRig {
    fv::MemorySettingsStore store;
    fv::CleanupProfilesPanel panel{store};
    QMessageBox::StandardButton answer = QMessageBox::Cancel;
    int asked = 0;

    PanelRig() {
        panel.question = [this](QWidget*, const QString&, const QString&,
                                QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            ++asked;
            return answer;
        };
    }
    std::vector<CleanupProfile> profiles() {
        return fv::text::load_cleanup_profiles(*store.snapshot());
    }
};

QString cancelHotkey() {
    return qs(fv::default_hotkeys().at("cancel").get<std::string>());
}

} // namespace

// from tests/test_cleanup_profiles.py::test_editor_save_duplicate_and_delete_update_library
TEST_CASE("test_editor_save_duplicate_and_delete_update_library",
          "[test_cleanup_profiles][widget]") {
    PanelRig rig;
    int changed = 0;
    QObject::connect(&rig.panel, &fv::CleanupProfilesPanel::profilesChanged,
                     [&changed] { ++changed; });
    rig.panel.newProfile();
    rig.panel.nameEdit()->setText(QStringLiteral("Release notes"));
    rig.panel.instructionsEdit()->setPlainText(QStringLiteral("Make concise bullets."));
    rig.panel.hotkeyInput()->setHotkey(QStringLiteral("ctrl+alt+f9"));
    CHECK(rig.panel.saveProfile());
    const CleanupProfile original = rig.profiles().back();
    rig.panel.duplicateProfile();
    CHECK(rig.panel.hotkeyInput()->hotkey().isEmpty());
    CHECK(rig.panel.saveProfile());
    const auto profiles = rig.profiles();
    CHECK(profiles.back().id != original.id);
    CHECK(profiles.back().name == "Release notes copy");
    rig.answer = QMessageBox::Yes;
    rig.panel.deleteProfile();
    CHECK(rig.profiles().back() == original);
    CHECK(changed == 3);
}

// from tests/test_cleanup_profiles.py::test_editor_reports_conflict_and_keeps_draft
TEST_CASE("test_editor_reports_conflict_and_keeps_draft", "[test_cleanup_profiles][widget]") {
    PanelRig rig;
    rig.panel.nameEdit()->setText(QStringLiteral("Ticket"));
    rig.panel.hotkeyInput()->setHotkey(cancelHotkey());
    CHECK_FALSE(rig.panel.saveProfile());
    CHECK(rig.panel.message()->text().contains(QStringLiteral("already used")));
    CHECK(rig.panel.nameEdit()->text() == QStringLiteral("Ticket"));
    CHECK(rig.profiles().front().name == "Support ticket");
}

// from
// tests/test_cleanup_profiles.py::test_save_while_switching_keeps_list_and_editor_on_destination
TEST_CASE("test_save_while_switching_keeps_list_and_editor_on_destination",
          "[test_cleanup_profiles][widget]") {
    PanelRig rig;
    rig.panel.nameEdit()->setText(QStringLiteral("Technical ticket"));
    rig.answer = QMessageBox::Save;
    rig.panel.profileList()->setCurrentRow(1);
    CHECK(rig.panel.nameEdit()->text() == QStringLiteral("Email"));
    CHECK(rig.panel.profileList()->currentRow() == 1);
    CHECK(rig.profiles().front().name == "Technical ticket");
}

// from tests/test_cleanup_profiles.py::test_cancel_switch_and_refresh_keep_unsaved_work
TEST_CASE("test_cancel_switch_and_refresh_keep_unsaved_work", "[test_cleanup_profiles][widget]") {
    PanelRig rig;
    rig.panel.nameEdit()->setText(QStringLiteral("Unfinished ticket"));
    rig.answer = QMessageBox::Cancel;
    rig.panel.profileList()->setCurrentRow(1);
    rig.panel.refresh();
    CHECK(rig.panel.nameEdit()->text() == QStringLiteral("Unfinished ticket"));
    CHECK(rig.panel.profileList()->currentRow() == 0);
}

// from tests/test_cleanup_profiles.py::test_capture_accepts_tab_without_moving_focus
TEST_CASE("test_capture_accepts_tab_without_moving_focus", "[test_cleanup_profiles][widget]") {
    fv::ProfileHotkeyInput field;
    field.beginCapture();
    QTest::keyClick(&field, Qt::Key_Tab, Qt::AltModifier);
    CHECK(field.hotkey() == QStringLiteral("alt+tab"));
    CHECK_FALSE(field.isCapturing());
}

// from tests/test_cleanup_profiles.py::test_leaving_settings_saves_a_valid_draft
TEST_CASE("test_leaving_settings_saves_a_valid_draft", "[test_cleanup_profiles][widget]") {
    PanelRig rig;
    rig.panel.nameEdit()->setText(QStringLiteral("Technical ticket"));
    CHECK(rig.panel.hasUnsavedChanges());
    CHECK(rig.panel.saveDraft());
    CHECK_FALSE(rig.panel.hasUnsavedChanges());
    CHECK(rig.profiles().front().name == "Technical ticket");
}

// from
// tests/test_cleanup_profiles.py::test_leaving_settings_asks_before_dropping_a_draft_that_cannot_save
TEST_CASE("test_leaving_settings_asks_before_dropping_a_draft_that_cannot_save",
          "[test_cleanup_profiles][widget]") {
    PanelRig rig;
    rig.panel.hotkeyInput()->setHotkey(cancelHotkey());
    // Nobody to ask (a hidden window): the draft stays in the editor.
    CHECK(rig.panel.saveDraft());
    CHECK(rig.panel.hasUnsavedChanges());
    rig.answer = QMessageBox::Cancel;
    CHECK_FALSE(rig.panel.saveDraft(true));
    CHECK(rig.panel.hasUnsavedChanges());
    rig.answer = QMessageBox::Discard;
    CHECK(rig.panel.saveDraft(true));
    CHECK_FALSE(rig.panel.hasUnsavedChanges());
}

// --- The page around the panel ----------------------------------------------------------------

TEST_CASE("the profiles page counts profiles, says they changed and opens the cleanup model",
          "[widget]") {
    PersonalizeRig rig;
    CHECK(rig.railValue(dest::cleanup_profiles) == QStringLiteral("2 profiles"));
    fv::CleanupProfilesPanel& panel = rig.profilesPanel();
    CHECK(rig.window().control("cleanup_profiles_panel") == &panel);

    panel.newProfile();
    panel.nameEdit()->setText(QStringLiteral("Release notes"));
    panel.instructionsEdit()->setPlainText(QStringLiteral("Make concise bullets."));
    CHECK(panel.saveProfile());
    CHECK(panel.message()->text() ==
          QStringLiteral("Saved Release notes. Available on Quick Record."));
    CHECK(rig.changes == QStringList{QStringLiteral("cleanup_profiles")});
    CHECK(rig.railValue(dest::cleanup_profiles) == QStringLiteral("3 profiles"));

    panel.hotkeyInput()->beginCapture();
    panel.hotkeyInput()->cancelCapture();
    CHECK(rig.captures == std::vector<bool>{true, false});

    panel.modelButton()->click();
    CHECK(rig.window().currentDestination() == qs(dest::cleanup));
}

TEST_CASE("leaving the profiles page saves a valid draft and keeps one that cannot be saved",
          "[widget]") {
    PersonalizeRig rig;
    fv::ProfilesPage& page = rig.profiles();
    fv::CleanupProfilesPanel& panel = *page.panel();
    panel.nameEdit()->setText(QStringLiteral("Technical ticket"));
    rig.window().selectDestination(qs(dest::general));
    CHECK(rig.savedProfiles().front().name == "Technical ticket");

    rig.window().selectDestination(qs(dest::cleanup_profiles));
    panel.hotkeyInput()->setHotkey(cancelHotkey());
    rig.window().selectDestination(qs(dest::general));
    CHECK(rig.asked.isEmpty());
    CHECK(panel.hasUnsavedChanges());
    CHECK(panel.message()->text().contains(QStringLiteral("already used")));
}

TEST_CASE("the profile editor goes under a 160 px list when the panel is narrow", "[widget]") {
    // cleanup_profiles_panel.resizeEvent (a13 A13-39). Inside a window of its own, so the
    // panel can be made narrower than its layout's minimum, as a narrow Settings page makes it.
    PanelRig rig;
    fv::CleanupProfilesPanel& panel = rig.panel;
    QWidget window;
    panel.setParent(&window);
    window.resize(900, 600);
    window.show();
    panel.setGeometry(0, 0, 900, 600);
    QCoreApplication::processEvents();
    CHECK(panel.columns()->direction() == QBoxLayout::LeftToRight);
    CHECK(panel.library()->maximumWidth() == fv::CleanupProfilesPanel::kLibraryWidth);
    CHECK(panel.library()->minimumWidth() == fv::CleanupProfilesPanel::kLibraryWidth);
    CHECK(panel.profileList()->maximumHeight() == QWIDGETSIZE_MAX);

    panel.setGeometry(0, 0, fv::CleanupProfilesPanel::kLibraryWidth + 40, 600);
    QCoreApplication::processEvents();
    CHECK(panel.columns()->direction() == QBoxLayout::TopToBottom);
    CHECK(panel.library()->minimumWidth() == 0);
    CHECK(panel.library()->maximumWidth() == QWIDGETSIZE_MAX);
    CHECK(panel.profileList()->maximumHeight() == 160);
    panel.setParent(nullptr);
}
