// The Hotkeys page's Actions group (ActionBindingSource), registered by the Automation feature on
// the window's real Hotkeys page beside the profile and transform rows
// (tests/test_hotkey_directory.py, the three tests that need saved actions). The page's global
// captures are fakes; a row's own field is driven by its signals, so nothing hooks the keyboard.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QLabel>
#include <QPushButton>

#include "actions_page_support.h"
#include "pyport/errors.h"
#include "settings/hotkeys.h"
#include "text/cleanup_profiles.h"
#include "text/transforms.h"
#include "ui_automation/action_bindings.h"
#include "ui_hotkeys/hotkey_bindings.h"
#include "ui_hotkeys/hotkey_row.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_hotkeys/profile_hotkey_input.h"

using fv::Json;
using namespace fvtest;

namespace {

Json action(const std::string& id, const std::string& name, const std::string& hotkey,
            bool enabled = true) {
    return Json{{"id", id},
                {"name", name},
                {"kind", "open_link"},
                {"params", {{"template", "https://example.com/?q={text}"}}},
                {"hotkey", hotkey},
                {"shaping", "cleanup"},
                {"spoken_names", Json::array()},
                {"description", ""},
                {"enabled", enabled}};
}

// The window on its Hotkeys page.
fv::HotkeysPage& hotkeysPage(ActionsRig& rig) {
    fv::SettingsDialog& dialog = rig.dialog(false);
    dialog.selectDestination(qs(dest::hotkeys));
    pump();
    auto* page = dynamic_cast<fv::HotkeysPage*>(dialog.pageContent(dest::hotkeys));
    REQUIRE(page != nullptr);
    return *page;
}

// _field: the page's box for the profile, transform or action called `name`.
fv::ProfileHotkeyInput* field(fv::HotkeysPage& page, const QString& name) {
    std::vector<fv::ProfileHotkeyInput*> found;
    for (auto* input : page.bindingShortcuts()->findChildren<fv::ProfileHotkeyInput*>()) {
        if (input->accessibleName() == name + QStringLiteral(" shortcut")) {
            found.push_back(input);
        }
    }
    INFO(name.toStdString());
    REQUIRE(found.size() == 1);
    return found.front();
}

QWidget* card(QWidget* widget) {
    while (widget != nullptr && qobject_cast<fv::ReflowCard*>(widget) == nullptr) {
        widget = widget->parentWidget();
    }
    REQUIRE(widget != nullptr);
    return widget;
}

QString note(QWidget* input) {
    auto* label = card(input)->findChild<QLabel*>(QStringLiteral("hotkeyShortcutNote"));
    REQUIRE(label != nullptr);
    return label->isHidden() ? QString() : label->text();
}

QPushButton* clearButton(QWidget* input) {
    auto* button = card(input)->findChild<QPushButton*>(QStringLiteral("hotkeyClearButton"));
    REQUIRE(button != nullptr);
    return button;
}

} // namespace

// from
// tests/test_hotkey_directory.py::test_profiles_transforms_and_actions_are_listed_with_their_shortcuts
TEST_CASE("test_profiles_transforms_and_actions_are_listed_with_their_shortcuts",
          "[test_hotkey_directory][widget]") {
    ActionsRig rig(Json{{"actions", Json::array({action("ask", "Ask the docs", "ctrl+alt+d")})}},
                   true);
    fv::HotkeysPage& page = hotkeysPage(rig);
    const Json settings = *rig.settings().snapshot();
    for (const auto& profile : fv::text::load_cleanup_profiles(settings)) {
        CHECK(field(page, qs(profile.name))->text() == display(profile.hotkey));
    }
    for (const auto& transform : fv::text::load_transforms(settings)) {
        CHECK(field(page, qs(transform.name))->text() == display(transform.hotkey));
    }
    fv::ProfileHotkeyInput* ask = field(page, QStringLiteral("Ask the docs"));
    CHECK(ask->text() == display("ctrl+alt+d"));
    CHECK(clearButton(ask)->isEnabled());
}

// from tests/test_hotkey_directory.py::test_clear_removes_an_action_shortcut
TEST_CASE("test_clear_removes_an_action_shortcut", "[test_hotkey_directory][widget]") {
    ActionsRig rig(Json{{"actions", Json::array({action("ask", "Ask the docs", "ctrl+alt+d")})}},
                   true);
    fv::HotkeysPage& page = hotkeysPage(rig);
    fv::ProfileHotkeyInput* ask = field(page, QStringLiteral("Ask the docs"));

    clearButton(ask)->click();

    CHECK(rig.saved("actions")[0]["hotkey"] == "");
    CHECK(ask->text().isEmpty());
    CHECK_FALSE(clearButton(ask)->isEnabled());
    REQUIRE_FALSE(rig.changes.isEmpty());
    CHECK(rig.changes.last() == QStringLiteral("actions"));
    CHECK(rig.current().messageText() == QStringLiteral("Ask the docs shortcut cleared."));
}

// from tests/test_hotkey_directory.py::test_rows_say_when_a_shortcut_does_nothing
TEST_CASE("test_rows_say_when_a_shortcut_does_nothing", "[test_hotkey_directory][widget]") {
    const std::string record = fv::default_hotkeys().at("record_toggle").get<std::string>();
    ActionsRig rig(
        Json{{"actions", Json::array({action("off", "Paused action", "ctrl+alt+o", false)})}},
        true);
    fv::HotkeysPage& page = hotkeysPage(rig);
    // A hand-edited file can hold a shortcut the app then skips.
    rig.settings().mutate([&record](Json& settings) {
        settings["text_transforms"] = Json::array({Json{{"id", "polish"},
                                                        {"name", "Polish"},
                                                        {"instruction", "Polish it."},
                                                        {"hotkey", record}}});
    });
    rig.current().refresh();

    CHECK(note(field(page, QStringLiteral("Polish"))) ==
          QStringLiteral("Recording uses this shortcut too, so it does nothing here."));
    CHECK(note(field(page, QStringLiteral("Paused action"))) ==
          QStringLiteral("This action is turned off, so its shortcut does nothing."));
}

TEST_CASE("the action source lists every saved action with its summary and switch", "[unit]") {
    fv::ActionBindingSource source;
    const Json settings{{"actions", Json::array({action("ask", "Ask the docs", "ctrl+alt+d"),
                                                 action("off", "Paused action", "", false)})}};
    const auto bindings = source.list(settings);
    REQUIRE(bindings.size() == 2);
    CHECK(bindings[0] == fv::hotkey_bindings::Binding{"action", "ask", "Ask the docs", "ctrl+alt+d",
                                                      "Opens example.com with your words.", true});
    CHECK(bindings[1].enabled == false);
    CHECK(bindings[1].detail == "Opens example.com with your words.");
}

TEST_CASE("the action source saves through the action editor's validation", "[unit]") {
    int saved = 0;
    fv::ActionBindingSource source([&saved] { ++saved; });
    Json settings{{"actions", Json::array({action("ask", "Ask the docs", "ctrl+alt+d"),
                                           action("two", "Second", "ctrl+alt+e")})}};
    CHECK(source.save(settings, "ask", "ctrl+alt+k"));
    CHECK(settings["actions"][0]["hotkey"] == "ctrl+alt+k");
    CHECK_FALSE(source.save(settings, "gone", "ctrl+alt+q"));
    try {
        source.save(settings, "two", "ctrl+alt+k");
        FAIL("a shortcut another action uses was saved");
    } catch (const fv::py::ValueError& refused) {
        CHECK(std::string(refused.what()) ==
              "That shortcut is already used by Ask the docs. Choose another.");
    }
    CHECK(settings["actions"][1]["hotkey"] == "ctrl+alt+e");
    source.saved();
    CHECK(saved == 1);
}
