// The app's Hotkeys page lists the profile and transform shortcuts once the composition registers
// them (tests/test_hotkey_directory.py's two tests that need both pages in one Settings window):
// the real feature graph offscreen, the Settings window the app opens, no hook installed.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <QLineEdit>
#include <QTextEdit>

#include "appkit/destinations.h"
#include "hotkeys/grammar.h"
#include "personalize_support.h"
#include "text/cleanup_profiles.h"
#include "text/transforms.h"
#include "ui_hotkeys/hotkey_bindings.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_personalize/cleanup_profiles_panel.h"
#include "ui_personalize/profiles_page.h"

using fv::Json;
using fvtest::ControllerRig;

namespace {

namespace dest = fv::destinations;

QString q(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

fv::HotkeysPage& hotkeysPage(fv::SettingsDialog& dialog) {
    auto* page = qobject_cast<fv::HotkeysPage*>(dialog.pageContent(dest::hotkeys));
    REQUIRE(page != nullptr);
    return *page;
}

// The row's shortcut field, by the item's name ("<name> shortcut").
fv::ProfileHotkeyInput* field(fv::HotkeysPage& page, const QString& name) {
    fv::ProfileHotkeyInput* match = nullptr;
    int count = 0;
    for (auto* found : page.bindingShortcuts()->findChildren<fv::ProfileHotkeyInput*>()) {
        if (found->accessibleName() == name + QStringLiteral(" shortcut")) {
            match = found;
            ++count;
        }
    }
    return count == 1 ? match : nullptr;
}

fv::text::Transform transformNamed(const Json& settings, const std::string& name) {
    for (const fv::text::Transform& transform : fv::text::load_transforms(settings)) {
        if (transform.name == name) {
            return transform;
        }
    }
    FAIL("no transform " << name);
    return {};
}

} // namespace

TEST_CASE("test_a_shortcut_changed_on_its_own_page_shows_on_hotkeys", "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    fv::SettingsDialog& dialog = fvtest::settingsDialog(rig);
    dialog.show();
    dialog.selectDestination(q(dest::commands));
    const fv::text::Transform polish = transformNamed(*rig.settings().snapshot(), "Polish");
    rig.settings().mutate([&polish](Json& settings) {
        fv::text::Transform changed = polish;
        changed.hotkey = "ctrl+alt+j";
        fv::text::save_transform(settings, changed);
    });

    dialog.selectDestination(q(dest::hotkeys));

    fv::ProfileHotkeyInput* polishField = field(hotkeysPage(dialog), QStringLiteral("Polish"));
    REQUIRE(polishField != nullptr);
    CHECK(polishField->text() ==
          q(fv::hotkeys::format_hotkey_display("ctrl+alt+j", fv::hotkeys::platform_vocabulary())));
    dialog.close();
}

TEST_CASE("test_a_profile_draft_takes_the_shortcut_set_on_hotkeys", "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    fv::SettingsDialog& dialog = fvtest::settingsDialog(rig);
    dialog.show();
    dialog.selectDestination(q(dest::cleanup_profiles));
    auto* profiles = dynamic_cast<fv::ProfilesPage*>(dialog.pageContent(dest::cleanup_profiles));
    REQUIRE(profiles != nullptr);
    fv::CleanupProfilesPanel& panel = *profiles->panel();
    CHECK(panel.nameEdit()->text() == QStringLiteral("Support ticket"));
    panel.instructionsEdit()->setPlainText(QStringLiteral("A short ticket."));

    dialog.selectDestination(q(dest::hotkeys));
    fv::ProfileHotkeyInput* ticket = field(hotkeysPage(dialog), QStringLiteral("Support ticket"));
    REQUIRE(ticket != nullptr);
    Q_EMIT ticket->captured(QStringLiteral("ctrl+alt+t"));
    // The row saved the shortcut on the profile (the starter is now a saved profile).
    const auto saved = fv::text::find_cleanup_profile(*rig.settings().snapshot(), "support-ticket");
    REQUIRE(saved.has_value());
    CHECK(saved->hotkey == "ctrl+alt+t");
    dialog.selectDestination(q(dest::cleanup_profiles));

    // The unsaved instructions stay, and saving them keeps the new shortcut.
    CHECK(panel.instructionsEdit()->toPlainText() == QStringLiteral("A short ticket."));
    CHECK(panel.hotkeyInput()->hotkey() == QStringLiteral("ctrl+alt+t"));
    CHECK(panel.saveProfile());
    bool found = false;
    for (const fv::text::CleanupProfile& profile :
         fv::text::load_cleanup_profiles(*rig.settings().snapshot())) {
        if (profile.name == "Support ticket") {
            found = true;
            CHECK(profile.instructions == "A short ticket.");
            CHECK(profile.hotkey == "ctrl+alt+t");
        }
    }
    CHECK(found);
    dialog.close();
}
