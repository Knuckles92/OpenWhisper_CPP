// AutomationFeature: the Automate chapter's entry point, registered in the generated feature
// list. What it provides and registers, and "AutomationFeature connects every signal"
// (architecture 8.5) for the Actions page's classes. The Hooks page (M5.17) and the API & MCP
// page (M5.21) register through this feature too; their classes join the check when they land.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

#include <QMetaMethod>

#include "actions_page_support.h"
#include "ui_automation/action_bindings.h"
#include "ui_automation/action_list.h"
#include "ui_automation/automation_feature.h"

using fv::Json;
using namespace fvtest;

namespace {

struct SignalProbe : QObject {
    using QObject::isSignalConnected;
};

bool connected(const QObject& object, const QMetaMethod& signal) {
    return (object.*(&SignalProbe::isSignalConnected))(signal);
}

// Signals a class declares that nothing on the Actions page needs, by design.
const std::map<std::string, std::set<std::string>>& unusedByDesign() {
    static const std::map<std::string, std::set<std::string>> unused{
        // Clear emits captured("") first, which the editor and the card already save.
        {"fv::ShortcutField", {"cleared"}},
        // The field saves through the host and pauses hotkeys itself; the page only follows
        // a saved change.
        {"fv::RouteShortcutField", {"captured", "captureChanged", "cleared"}},
        // A chosen program is read when the draft is; nothing follows each choice.
        {"fv::PathField", {"changed"}},
        // The editor and the card read the shaping and the agent fields when they save.
        {"fv::ShapingField", {"changed"}},
    };
    return unused;
}

// Every signal `object`'s own class declares is connected, unless it is unused by design (for
// its class, or for this one instance in `alsoUnused`). Returns how many it declares.
int checkEverySignalConnected(const QObject& object, const QMetaObject& own,
                              const std::set<std::string>& alsoUnused = {}) {
    int declared = 0;
    const auto skipped = unusedByDesign().find(own.className());
    for (int index = own.methodOffset(); index < own.methodCount(); ++index) {
        const QMetaMethod method = own.method(index);
        if (method.methodType() != QMetaMethod::Signal) {
            continue;
        }
        ++declared;
        const std::string name = method.name().toStdString();
        INFO(own.className() << "::" << name);
        if ((skipped != unusedByDesign().end() && skipped->second.contains(name)) ||
            alsoUnused.contains(name)) {
            continue;
        }
        CHECK(connected(object, method));
    }
    return declared;
}

} // namespace

TEST_CASE("the automation feature is named and requires nothing", "[unit]") {
    fv::AutomationFeature feature;
    CHECK(feature.id() == "automation");
    CHECK(fv::AutomationFeature::kId == "automation");
    CHECK(feature.required_features().empty());
}

TEST_CASE("the automation feature registers the Actions page with its rail, search and controls",
          "[widget]") {
    ActionsRig rig(
        Json{{"actions", Json::array({Json{{"id", "a1"},
                                           {"name", "Ask Claude"},
                                           {"kind", "open_link"},
                                           {"params", {{"template", "claude://x?q={text}"}}}}})}});
    const fv::SettingsPageSpec* spec = rig.context().settingsPages().page(dest::actions);
    REQUIRE(spec != nullptr);
    CHECK(spec->title == QStringLiteral("Actions"));
    CHECK(spec->subtitle == QStringLiteral("Send what you say somewhere other than the cursor: a "
                                           "chat, your agent, or your own tools."));
    CHECK(spec->railValue(*rig.settings().snapshot()) == QStringLiteral("1 action"));
    REQUIRE(spec->searchFields.size() == 5);
    CHECK(spec->searchFields[0].target == "actions_agent_card");
    CHECK(spec->searchFields[0].label == QStringLiteral("Agent hotkey"));
    CHECK(spec->searchFields[4].description ==
          QStringLiteral("Record, then send your words to the action they name, or paste them as "
                         "usual."));
    CHECK(spec->controls == fv::actions_page::control_names());
    CHECK(spec->controls.size() == 8);
}

TEST_CASE("the automation feature wires the app's Settings window when it is made", "[widget]") {
    ActionsRig rig(Json::object(), true);
    fv::SettingsDialog& dialog = rig.context().service<fv::SettingsWindow>().ensureDialog();
    auto* wiring = dialog.findChild<fv::ActionBindingsWiring*>();
    REQUIRE(wiring != nullptr);
    CHECK(wiring->registeredPage() == nullptr);
    dialog.selectDestination(qs(dest::hotkeys));
    pump();
    auto* hotkeys = dynamic_cast<fv::HotkeysPage*>(dialog.pageContent(dest::hotkeys));
    REQUIRE(hotkeys != nullptr);
    CHECK(wiring->registeredPage() == hotkeys);
    CHECK(hotkeys->bindingShortcuts()->sources().contains("action"));
}

TEST_CASE("the automation feature gives the session one action relay", "[widget]") {
    fv::testkit::TempDataRoot root;
    fv::Paths paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    fv::AppServices services;
    services.identity = &fv::identity();
    services.paths = &paths;
    fv::AppContext without(services);
    fv::AutomationFeature feature;
    feature.provide(without);
    // No io executor or credential store: no relay, and the page makes its own.
    CHECK(without.find<fv::ActionRelay>() == nullptr);
}

TEST_CASE("AutomationFeature connects every signal", "[widget]") {
    ActionsRig rig(
        Json{{"actions", Json::array({Json{{"id", "a1"},
                                           {"name", "Ask Claude"},
                                           {"kind", "open_link"},
                                           {"params", {{"template", "claude://x?q={text}"}}}}})}},
        true);
    fv::SettingsDialog& dialog = rig.dialog();
    fv::ActionsPage& page = rig.page();
    CHECK(checkEverySignalConnected(page, fv::ActionsPage::staticMetaObject) == 0);
    CHECK(checkEverySignalConnected(*page.agentCard(), fv::AgentHotkeyCard::staticMetaObject) == 4);
    CHECK(checkEverySignalConnected(*page.panel(), fv::ActionsPanel::staticMetaObject) == 4);
    fv::ActionEditor& editor = *page.panel()->editor();
    CHECK(checkEverySignalConnected(editor, fv::ActionEditor::staticMetaObject) == 4);
    CHECK(checkEverySignalConnected(*page.panel()->list(), fv::ActionList::staticMetaObject) == 4);
    fv::ActionCard* card = page.panel()->list()->card(QStringLiteral("a1"));
    REQUIRE(card != nullptr);
    CHECK(checkEverySignalConnected(*card, fv::ActionCard::staticMetaObject) == 3);
    CHECK(checkEverySignalConnected(*page.panel()->gallery(),
                                    fv::PresetGallery::staticMetaObject) == 2);
    for (const fv::PresetTile* tile : page.panel()->gallery()->tiles()) {
        CHECK(checkEverySignalConnected(*tile, fv::PresetTile::staticMetaObject) == 1);
    }
    CHECK(checkEverySignalConnected(*editor.shortcut(), fv::ShortcutField::staticMetaObject) == 3);
    CHECK(checkEverySignalConnected(*page.agentCard()->shortcut(),
                                    fv::ShortcutField::staticMetaObject) == 3);
    CHECK(checkEverySignalConnected(*editor.shaping(), fv::ShapingField::staticMetaObject) == 1);
    CHECK(checkEverySignalConnected(*editor.programField(), fv::PathField::staticMetaObject) == 1);
    // The card saves on each change of its agent fields; the editor reads its own when it saves.
    CHECK(checkEverySignalConnected(*page.agentCard()->fields(),
                                    fv::AgentFields::staticMetaObject) == 2);
    CHECK(checkEverySignalConnected(*editor.agentFields(), fv::AgentFields::staticMetaObject,
                                    {"changed"}) == 2);
    for (fv::AgentFields* fields : {editor.agentFields(), page.agentCard()->fields()}) {
        CHECK(checkEverySignalConnected(*fields->chooser(), fv::AgentChooser::staticMetaObject) ==
              1);
    }
    CHECK(checkEverySignalConnected(*page.routingStatusLine(), fv::StatusLine::staticMetaObject) ==
          1);
    CHECK(checkEverySignalConnected(*page.routeShortcut(),
                                    fv::RouteShortcutField::staticMetaObject) == 4);
    auto* relay = rig.context().find<fv::ActionRelay>();
    REQUIRE(relay != nullptr);
    CHECK(checkEverySignalConnected(*relay, fv::ActionRelay::staticMetaObject) == 3);

    // The Hotkeys page's action rows come from the source the feature registered on it.
    dialog.selectDestination(qs(dest::hotkeys));
    pump();
    auto* hotkeys = dynamic_cast<fv::HotkeysPage*>(dialog.pageContent(dest::hotkeys));
    REQUIRE(hotkeys != nullptr);
    CHECK(hotkeys->bindingShortcuts()->sources().contains("action"));
    auto* wiring = dialog.findChild<fv::ActionBindingsWiring*>();
    REQUIRE(wiring != nullptr);
    CHECK(wiring->registeredPage() == hotkeys);
}
