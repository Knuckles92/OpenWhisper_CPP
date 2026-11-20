// The Profiles and Commands editors of the app's Settings window check a new shortcut against the
// saved actions as well (hotkey_conflicts.hotkey_conflict reads load_actions(settings), which the
// editors reach through the controller's personalize wiring).
#include <catch2/catch_test_macros.hpp>

#include <QLineEdit>
#include <QTextEdit>

#include "appkit/destinations.h"
#include "controller_support.h"
#include "personalize_support.h"
#include "text/transforms.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_personalize/cleanup_profiles_panel.h"
#include "ui_personalize/transforms_panel.h"
#include "ui_settings/settings_dialog.h"
#include "ui_widgets/wrapped_label.h"

using fvtest::ControllerRig;
using Json = fv::Json;

namespace {

fvtest::RigOptions withAction(fvtest::MemoryKeys& keys) {
    fvtest::RigOptions options = fvtest::personalizeOptions(keys, nullptr);
    options.settings["actions"] =
        Json::array({Json{{"id", "ask"},
                          {"name", "Ask the docs"},
                          {"kind", "open_link"},
                          {"params", {{"template", "https://example.com/?q={text}"}}},
                          {"hotkey", "ctrl+alt+d"},
                          {"enabled", true}}});
    return options;
}

template <typename Panel> Panel* openPanel(ControllerRig& rig, std::string_view destination) {
    fv::SettingsDialog& dialog = fvtest::settingsDialog(rig);
    dialog.selectDestination(QString::fromUtf8(destination.data(), destination.size()));
    auto* panel = dialog.findChild<Panel*>();
    REQUIRE(panel != nullptr);
    return panel;
}

const QString kTaken =
    QStringLiteral("That shortcut is already used by Ask the docs. Choose another.");

} // namespace

TEST_CASE("a transform cannot take a saved action's shortcut", "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(withAction(keys));
    auto* panel = openPanel<fv::TransformsPanel>(rig, fv::destinations::commands);

    panel->newTransform();
    panel->nameEdit()->setText(QStringLiteral("Shorten"));
    panel->instructionEdit()->setPlainText(QStringLiteral("Make it shorter."));
    panel->hotkeyInput()->setHotkey(QStringLiteral("ctrl+alt+d"));

    CHECK_FALSE(panel->saveTransform());
    CHECK(panel->message()->text() == kTaken);
    for (const auto& transform : fv::text::load_transforms(*rig.settings().snapshot())) {
        CHECK(transform.name != "Shorten");
    }
}

TEST_CASE("a cleanup profile cannot take a saved action's shortcut", "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(withAction(keys));
    auto* panel = openPanel<fv::CleanupProfilesPanel>(rig, fv::destinations::cleanup_profiles);

    panel->newProfile();
    panel->nameEdit()->setText(QStringLiteral("Ticket"));
    panel->instructionsEdit()->setPlainText(QStringLiteral("Write a ticket."));
    panel->hotkeyInput()->setHotkey(QStringLiteral("ctrl+alt+d"));

    CHECK_FALSE(panel->saveProfile());
    CHECK(panel->message()->text() == kTaken);
}
