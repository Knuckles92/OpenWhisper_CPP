// Sweep S2 (open-items A8, A11, A27): how Home and the Settings window follow OpenWhisper's
// settings_dialog.py and ui_controller.py - Home's shortcut row after a save on the Hotkeys page,
// open_settings_dialog against open_settings_destination("overview"), and the order in which
// leaving Settings asks the editors about their drafts.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>

#include "home_support.h"
#include "settings/hotkeys.h"
#include "settings/keys.h"
#include "settings_support.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_settings/settings_window.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fvtest::HomeRig;
using fvtest::qs;
using fvtest::SettingsRig;
namespace dest = fv::destinations;
namespace keys = fv::keys;

namespace {

fv::HotkeysPage& hotkeysPage(fv::SettingsDialog& dialog) {
    dialog.selectDestination(qs(dest::hotkeys));
    auto* page = dynamic_cast<fv::HotkeysPage*>(dialog.pageContent(dest::hotkeys));
    REQUIRE(page != nullptr);
    return *page;
}

} // namespace

// _update_hotkey_displays calls home.refresh_shortcut() on every save, whichever page made it.
TEST_CASE("a shortcut saved on the Hotkeys page shows on Home at once", "[widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    fv::HomePage& home = rig.home();
    fv::HotkeysPage& page = hotkeysPage(dialog);
    REQUIRE(dialog.currentDestination() == qs(dest::hotkeys));
    const QString before = home.shortcut()->hotkey();
    const std::string chosen = "ctrl+alt+shift+f9";
    REQUIRE(before != qs(chosen));
    CHECK(page.setStandardHotkey("record_toggle", chosen).isEmpty());
    // Home is not the page shown, and its row already says what was saved.
    CHECK(dialog.currentDestination() == qs(dest::hotkeys));
    CHECK(home.shortcut()->hotkey() == qs(chosen));
}

// _on_hands_free_latch_toggled: the latch changes what Home says the shortcut does.
TEST_CASE("the hands-free latch toggled on the Hotkeys page changes Home's line at once",
          "[widget]") {
    HomeRig rig(
        fv::Json{{"recording_trigger_mode", "push_hold"}, {"recording_hands_free_latch", false}});
    fv::SettingsDialog& dialog = rig.dialog();
    fv::HomePage& home = rig.home();
    CHECK(home.shortcutDescription()->text() ==
          QStringLiteral("Hold to record; release to stop and transcribe."));
    fv::HotkeysPage& page = hotkeysPage(dialog);
    page.handsFreeLatchSwitch()->setChecked(true);
    CHECK(rig.saved(keys::recording_hands_free_latch.key()) == fv::Json(true));
    CHECK(dialog.currentDestination() == qs(dest::hotkeys));
    CHECK(home.shortcutDescription()->text() ==
          QStringLiteral("Hold to record, or double-tap to keep recording hands-free."));
}

// open_settings_dialog scrolls Home to its top (show_home); open_settings_destination("overview"),
// which the main window's settings_destination_requested reaches, only selects it.
TEST_CASE("only opening Settings itself scrolls Home to the top", "[widget]") {
    HomeRig rig;
    fv::SettingsWindow window(rig.context());
    window.raiseDialog = [](fv::SettingsDialog& dialog) {
        dialog.show();
    };
    window.open();
    fv::SettingsDialog* dialog = window.dialog();
    REQUIRE(dialog != nullptr);
    const std::unique_ptr<fv::SettingsDialog> owned(dialog);
    dialog->resize(fv::SettingsDialog::kMinimumSize);
    fvtest::processEvents(4);
    QScrollArea* scroll = dialog->pageScroll(dest::overview);
    REQUIRE(scroll != nullptr);
    QScrollBar* bar = scroll->verticalScrollBar();
    REQUIRE(bar->maximum() > 0);
    bar->setValue(bar->maximum());
    const int scrolled = bar->value();
    REQUIRE(scrolled > 0);

    dialog->selectDestination(qs(dest::general));
    window.open(qs(dest::overview));
    CHECK(dialog->currentDestination() == qs(dest::overview));
    CHECK(bar->value() == scrolled);

    dialog->selectDestination(qs(dest::general));
    window.open();
    CHECK(dialog->currentDestination() == qs(dest::overview));
    CHECK(bar->value() == 0);
    dialog->close();
}

// _save_page_drafts: the page modules (Commands among them) are asked first and the Profiles
// panel, which the dialog builds itself, last.
TEST_CASE("leaving Settings asks Commands about its draft before Profiles", "[widget]") {
    SettingsRig rig;
    // A page whose draft cannot be saved yet: kept only when the user is asked (save_draft's
    // `not ask` returns True), as the editors' save_draft does.
    struct Drafts : QWidget, fv::SettingsPageModule {
        Drafts(std::vector<std::string>& asked, std::string key, QWidget* parent)
            : QWidget(parent), asked_(asked), key_(std::move(key)) {}
        bool saveDrafts(bool ask) override {
            asked_.push_back(ask ? key_ + " (asked)" : key_);
            return !(keep && ask);
        }
        bool keep = false;

    private:
        std::vector<std::string>& asked_;
        std::string key_;
    };
    std::vector<std::string> asked;
    QPointer<Drafts> commands;
    QPointer<Drafts> profiles;
    rig.registerStandIns([&](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::commands) {
            spec.create = [&](QWidget* parent) -> QWidget* {
                commands = new Drafts(asked, "commands", parent);
                return commands;
            };
        } else if (spec.destination == dest::cleanup_profiles) {
            spec.create = [&](QWidget* parent) -> QWidget* {
                profiles = new Drafts(asked, "profiles", parent);
                return profiles;
            };
        }
    });
    auto dialog = rig.dialog();
    dialog->show();
    dialog->ensurePage(dest::commands);
    dialog->ensurePage(dest::cleanup_profiles);
    REQUIRE(commands != nullptr);
    REQUIRE(profiles != nullptr);

    dialog->selectDestination(qs(dest::general));
    CHECK(asked == std::vector<std::string>{"commands", "profiles"});

    // Both have drafts that cannot be saved: Commands is asked first and shown (showing it saves
    // the other drafts without asking).
    asked.clear();
    commands->keep = true;
    profiles->keep = true;
    dialog->close();
    CHECK(dialog->isVisible());
    REQUIRE_FALSE(asked.empty());
    CHECK(asked.front() == "commands (asked)");
    CHECK(dialog->rail()->currentKey() == qs(dest::commands));

    // Commands' draft dealt with: Profiles is asked after it.
    asked.clear();
    commands->keep = false;
    dialog->close();
    CHECK(dialog->isVisible());
    REQUIRE(asked.size() >= 2);
    CHECK(asked[0] == "commands (asked)");
    CHECK(asked[1] == "profiles (asked)");
    CHECK(dialog->rail()->currentKey() == qs(dest::cleanup_profiles));

    profiles->keep = false;
    dialog->close();
    CHECK_FALSE(dialog->isVisible());
}
