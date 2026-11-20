// Settings > Automate > Actions: the agent card, your actions, the editor and Route by voice
// (tests/test_automation_ui_actions_page.py and test_automation_fix_ui_actions_page.py), inside a
// real Settings window over the fakes of actions_page_support.h. Where Python asserts at once
// on something that comes back from a worker or through the relay (a status, a Try it answer),
// the event loop turns first: the C++ relay always delivers queued, and Route by voice's status
// is worked out off the GUI thread because it reads the credential store.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <latch>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QAbstractButton>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPoint>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "actions_page_support.h"
#include "appcore/settings_search.h"
#include "automation/action_copy.h"
#include "golden/golden.h"
#include "settings/keys.h"
#include "ui_automation/action_gallery.h"
#include "ui_automation/action_list.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_settings/settings_search_palette.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/empty_state.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using fv::automation::Action;
using fv::automation::ActionResult;
using fv::automation::RoutingStatus;
using namespace fvtest;

namespace {

const Json kAskClaude{{"id", "a1"},
                      {"name", "Ask Claude"},
                      {"kind", "open_link"},
                      {"params", {{"template", "claude://claude.ai/new?q={text}"}}},
                      {"hotkey", "ctrl+alt+c"},
                      {"spoken_names", Json::array({"Claude"})}};
const Json kHook{
    {"id", "h1"},
    {"name", "Home Assistant"},
    {"kind", "webhook"},
    {"params", {{"url", "http://homeassistant.local/api/webhook/voice"}, {"output", "notice"}}}};
const Json kAgent{{"id", "agent"},
                  {"name", "Agent hotkey"},
                  {"kind", "agent"},
                  {"params", {{"agent", "claude_code"}}},
                  {"hotkey", "ctrl+alt+shift+space"},
                  {"spoken_names", Json::array({"Claude Code"})},
                  {"description", "Coding tasks for Claude Code"}};

Json withActions(std::vector<Json> actions, Json values = Json::object()) {
    values["actions"] = Json(std::move(actions));
    return values;
}

Json agentWith(const Json& params) {
    Json agent = kAgent;
    agent["params"] = params;
    return agent;
}

std::string text(const Json& value) {
    return value.is_string() ? value.get<std::string>() : std::string();
}

// The gallery tile's starting point, as Python's panel.gallery.tile(id).preset.
fv::automation::Preset galleryPreset(fv::ActionsPanel& panel, const char* id) {
    fv::PresetTile* tile = panel.gallery()->tile(QString::fromLatin1(id));
    REQUIRE(tile != nullptr);
    return tile->preset();
}

void startFrom(fv::ActionsPanel& panel, const char* id) {
    panel.addAction();
    panel.startFromPreset(galleryPreset(panel, id));
}

QString statusText(fv::ActionsPage& page) {
    return page.routingStatusLine()->message()->text();
}

// change_stored: a saved action changed behind the page's back, as another window would.
void changeStored(ActionsRig& rig, const std::string& id,
                  const std::function<void(Action&)>& change) {
    rig.settings().mutate([&](Json& settings) {
        std::optional<Action> action = fv::automation::find_action(settings, id);
        REQUIRE(action.has_value());
        change(*action);
        fv::automation::save_action(settings, *action);
    });
}

QStringList actionsTitles(fv::SettingsDialog& dialog) {
    QStringList titles;
    for (const fv::appcore::SearchEntry& entry : fv::buildSearchIndex(dialog)) {
        if (entry.destination == dest::actions) {
            titles << QString::fromStdString(entry.title);
        }
    }
    return titles;
}

const QStringList kPresetNames{QStringLiteral("Webhook"), QStringLiteral("Run a program"),
                               QStringLiteral("Copy only"), QStringLiteral("Send to my scripts"),
                               QStringLiteral("Your own link")};

bool containsAll(const QStringList& titles, const QStringList& wanted) {
    for (const QString& title : wanted) {
        if (!titles.contains(title)) {
            return false;
        }
    }
    return true;
}

bool containsAny(const QStringList& titles, const QStringList& unwanted) {
    for (const QString& title : unwanted) {
        if (titles.contains(title)) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---- building ----

// from tests/test_automation_ui_actions_page.py::test_the_page_builds_only_when_shown
TEST_CASE("test_the_page_builds_only_when_shown", "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    fv::SettingsDialog& dialog = rig.dialog(false);
    CHECK_FALSE(dialog.isBuilt(dest::actions));
    dialog.selectDestination(qs(dest::actions));
    CHECK(dialog.isBuilt(dest::actions));
    for (const std::string& name : fv::actions_page::control_names()) {
        INFO(name);
        CHECK(rig.page().namedControl(name) != nullptr);
        CHECK(dialog.control(name) == rig.page().namedControl(name));
    }
    for (const fv::actions_page::SearchField& field : fv::actions_page::search_fields()) {
        INFO(field.target);
        CHECK(rig.context().settingsPages().controlDestination(field.target) == dest::actions);
        CHECK_FALSE(field.title.empty());
    }
}

// from
// tests/test_automation_ui_actions_page.py::test_rail_value_names_the_agent_hotkey_and_counts_the_rest
TEST_CASE("test_rail_value_names_the_agent_hotkey_and_counts_the_rest",
          "[test_automation_ui_actions_page][unit]") {
    using fv::actions_page::rail_value;
    const Json agent{{"id", "agent"}, {"name", "Agent hotkey"}, {"kind", "agent"}};
    CHECK(rail_value(withActions({})) == "None yet");
    CHECK(rail_value(withActions({agent})) == "Agent hotkey set");
    CHECK(rail_value(withActions({kAskClaude})) == "1 action");
    CHECK(rail_value(withActions({agent, kAskClaude, kHook})) == "Agent hotkey set · 2 actions");
}

// ---- your actions ----

// from tests/test_automation_ui_actions_page.py::test_an_empty_page_invites_the_first_action
TEST_CASE("test_an_empty_page_invites_the_first_action",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    fv::ActionsPanel& panel = rig.panel();
    CHECK(panel.list()->emptyState()->isVisible());
    CHECK_FALSE(panel.addButton()->isVisible());
    panel.list()->emptyAddButton()->click();
    CHECK(panel.gallery()->isVisible());
    for (const fv::PresetTile* tile : panel.gallery()->tiles()) {
        CHECK(tile->preset().id != "agent");
    }
}

// from
// tests/test_automation_ui_actions_page.py::test_adding_from_a_preset_saves_it_and_tells_the_app
TEST_CASE("test_adding_from_a_preset_saves_it_and_tells_the_app",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    fv::ActionsPanel& panel = rig.panel();
    panel.addAction();
    fv::PresetTile* tile = panel.gallery()->tile(QStringLiteral("claude-chat"));
    REQUIRE(tile != nullptr);
    Q_EMIT tile->clicked(tile->preset());
    fv::ActionEditor& editor = rig.editor();
    CHECK(editor.isVisible());
    CHECK(editor.nameEdit()->text() == QStringLiteral("Ask Claude"));
    CHECK(editor.templateEdit()->text() == QStringLiteral("claude://claude.ai/new?q={text}"));
    CHECK(editor.preview()->text().contains(
        QStringLiteral("claude://claude.ai/new?q=What%27s%20on")));
    editor.spokenEdit()->setText(QStringLiteral("Claude, ask Claude"));
    Q_EMIT editor.shortcut()->captured(QStringLiteral("ctrl+alt+c"));
    CHECK(editor.save());

    const std::vector<Action> actions = rig.actions();
    REQUIRE(actions.size() == 1);
    const Action& saved = actions[0];
    CHECK(saved.name == "Ask Claude");
    CHECK(saved.kind == "open_link");
    CHECK(saved.hotkey == "ctrl+alt+c");
    CHECK(saved.spoken_names == std::vector<std::string>{"Claude", "ask Claude"});
    CHECK(text(saved.param("template")) == "claude://claude.ai/new?q={text}");
    CHECK(rig.changes == QStringList{QStringLiteral("actions")});
    CHECK(panel.list()->isVisible());
    CHECK_FALSE(editor.isVisible());
    CHECK(panel.list()->card(qs(saved.id)) != nullptr);
    CHECK(panel.notice()->text().startsWith(QStringLiteral("Saved Ask Claude. Press ")));
    CHECK(fv::actions_page::rail_value(*rig.settings().snapshot()) == "1 action");
}

// from tests/test_automation_ui_actions_page.py::test_adding_the_same_preset_twice_gets_a_new_name
TEST_CASE("test_adding_the_same_preset_twice_gets_a_new_name",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAskClaude}));
    rig.dialog();
    fv::ActionsPanel& panel = rig.panel();
    startFrom(panel, "claude-chat");
    CHECK(rig.editor().nameEdit()->text() == QStringLiteral("Ask Claude 2"));
    CHECK(rig.editor().save());
    const std::vector<Action> actions = rig.actions();
    REQUIRE(actions.size() == 2);
    CHECK(actions[0].name == "Ask Claude");
    CHECK(actions[1].name == "Ask Claude 2");
}

// from tests/test_automation_ui_actions_page.py::test_the_web_preset_carries_its_caveat
TEST_CASE("test_the_web_preset_carries_its_caveat", "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    startFrom(rig.panel(), "chatgpt-web");
    CHECK(rig.editor().presetNote()->isVisible());
    CHECK(rig.editor().presetNote()->text().contains(QStringLiteral("doesn't document")));
}

// from
// tests/test_automation_ui_actions_page.py::test_reasons_an_action_cant_be_saved_show_in_the_editor
TEST_CASE("test_reasons_an_action_cant_be_saved_show_in_the_editor",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAskClaude}));
    rig.dialog();
    startFrom(rig.panel(), "own-link");
    fv::ActionEditor& editor = rig.editor();
    CHECK_FALSE(editor.save());
    CHECK(editor.message()->text() == QStringLiteral("Enter the link to open."));
    editor.templateEdit()->setText(QStringLiteral("javascript:alert({text})"));
    CHECK(editor.preview()->text() ==
          QStringLiteral("Links that start with javascript: can't be used."));
    CHECK_FALSE(editor.save());
    CHECK(editor.message()->text() ==
          QStringLiteral("Links that start with javascript: can't be used."));
    editor.templateEdit()->setText(QStringLiteral("https://www.perplexity.ai/search"));
    CHECK(editor.preview()->text() == QStringLiteral("Add {text} where your words should go."));
    editor.templateEdit()->setText(QStringLiteral("https://www.perplexity.ai/search?q={text}"));
    editor.nameEdit()->setText(QStringLiteral("ask claude"));
    CHECK_FALSE(editor.save());
    CHECK(editor.message()->text() == QStringLiteral("An action with that name already exists."));
    editor.nameEdit()->setText(QStringLiteral("Perplexity"));
    editor.spokenEdit()->setText(QStringLiteral("Claude"));
    CHECK_FALSE(editor.save());
    CHECK(editor.message()->text() ==
          QStringLiteral("\"Claude\" already names Ask Claude. Choose another name."));
    const std::vector<Action> actions = rig.actions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].name == "Ask Claude");
    CHECK(rig.changes.isEmpty());
}

// from tests/test_automation_ui_actions_page.py::test_a_taken_shortcut_is_refused_with_its_owner
TEST_CASE("test_a_taken_shortcut_is_refused_with_its_owner",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAskClaude}));
    rig.dialog();
    startFrom(rig.panel(), "copy");
    fv::ActionEditor& editor = rig.editor();
    Q_EMIT editor.shortcut()->captured(QStringLiteral("ctrl+alt+k"));
    CHECK(editor.shortcut()->hotkey() == QStringLiteral("ctrl+alt+k"));
    editor.shortcut()->input()->beginCapture();
    editor.shortcut()->input()->cancelCapture();
    Q_EMIT editor.shortcut()->captured(QStringLiteral("alt+ctrl+c"));
    CHECK(editor.message()->text() ==
          QStringLiteral("That shortcut is already used by Ask Claude. Choose another."));
    CHECK(editor.shortcut()->hotkey() == QStringLiteral("ctrl+alt+k"));
    Q_EMIT editor.shortcut()->captured(QStringLiteral("ctrl+alt+m"));
    CHECK(editor.message()->text() ==
          QStringLiteral("That shortcut is already used by Minimize to tray. Choose another."));
    Q_EMIT editor.shortcut()->captured(QStringLiteral("ctrl+alt+j"));
    CHECK(editor.message()->text().isEmpty());
    CHECK(editor.save());
    const auto saved = rig.stored(fv::qt::toUtf8(editor.actionId()));
    REQUIRE(saved.has_value());
    CHECK(saved->hotkey == "ctrl+alt+j");
}

// from tests/test_automation_ui_actions_page.py::test_capturing_a_shortcut_pauses_the_app_hotkeys
TEST_CASE("test_capturing_a_shortcut_pauses_the_app_hotkeys",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    startFrom(rig.panel(), "copy");
    rig.editor().shortcut()->input()->beginCapture();
    rig.editor().shortcut()->input()->cancelCapture();
    CHECK(rig.captures == std::vector<bool>{true, false});
}

// from tests/test_automation_ui_actions_page.py::test_the_enabled_switch_saves_at_once
TEST_CASE("test_the_enabled_switch_saves_at_once", "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAskClaude}));
    rig.dialog();
    fv::ActionCard* card = rig.panel().list()->card(QStringLiteral("a1"));
    REQUIRE(card != nullptr);
    card->switchButton()->click();
    CHECK(rig.stored("a1")->enabled == false);
    CHECK(card->property("off").toBool());
    CHECK(rig.changes == QStringList{QStringLiteral("actions")});
    card->switchButton()->click();
    CHECK(rig.stored("a1")->enabled == true);
}

// from
// tests/test_automation_ui_actions_page.py::test_editing_keeps_the_id_and_cancel_keeps_the_saved_action
TEST_CASE("test_editing_keeps_the_id_and_cancel_keeps_the_saved_action",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAskClaude}));
    rig.dialog();
    fv::ActionsPanel& panel = rig.panel();
    panel.list()->card(QStringLiteral("a1"))->editLink()->click();
    fv::ActionEditor& editor = rig.editor();
    CHECK(editor.titleLabel()->text() == QStringLiteral("Edit Ask Claude"));
    editor.nameEdit()->setText(QStringLiteral("Claude chat"));
    editor.cancel();
    CHECK(rig.stored("a1")->name == "Ask Claude");
    panel.edit(QStringLiteral("a1"));
    editor.nameEdit()->setText(QStringLiteral("Claude chat"));
    CHECK(editor.save());
    const std::vector<Action> actions = rig.actions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].id == "a1");
    CHECK(actions[0].name == "Claude chat");
}

// from
// tests/test_automation_ui_actions_page.py::test_deleting_asks_first_and_forgets_a_webhooks_secret
TEST_CASE("test_deleting_asks_first_and_forgets_a_webhooks_secret",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAskClaude, kHook}));
    rig.dialog();
    rig.secrets.put("h1", "whsec_old");
    fv::ActionsPanel& panel = rig.panel();
    rig.answer = QMessageBox::Cancel;
    panel.list()->card(QStringLiteral("h1"))->deleteLink()->click();
    CHECK(rig.actions().size() == 2);
    rig.answer = QMessageBox::Yes;
    panel.list()->card(QStringLiteral("h1"))->deleteLink()->click();
    REQUIRE(rig.actions().size() == 1);
    CHECK(rig.actions()[0].id == "a1");
    CHECK(waitFor([&] { return !rig.secrets.get("h1").has_value(); }));
    CHECK_FALSE(rig.secrets.anyOnGuiThread());
    CHECK(panel.notice()->text() == QStringLiteral("Deleted Home Assistant."));
    CHECK(rig.changes == QStringList{QStringLiteral("actions")});
    panel.list()->card(QStringLiteral("a1"))->deleteLink()->click();
    pump();
    // Only a webhook has a secret to forget.
    CHECK(rig.secrets.calls() == 1);
}

// ---- the editor's kinds ----

// from tests/test_automation_ui_actions_page.py::test_shaping_offers_profiles_and_transforms
TEST_CASE("test_shaping_offers_profiles_and_transforms",
          "[test_automation_ui_actions_page][widget]") {
    const Json profile{{"id", "p1"}, {"name", "Ticket"}, {"instructions", "As a ticket."}};
    ActionsRig rig(Json{{"transcript_cleanup_profiles", Json::array({profile})}});
    rig.dialog();
    startFrom(rig.panel(), "copy");
    fv::ShapingField& shaping = *rig.editor().shaping();
    CHECK_FALSE(shaping.combo()->isVisible());
    shaping.bar()->buttons()[2]->click();
    CHECK(shaping.combo()->isVisible());
    CHECK(shaping.combo()->currentText() == QStringLiteral("Ticket"));
    CHECK(shaping.value() == QStringLiteral("profile:p1"));
    shaping.bar()->buttons()[3]->click();
    CHECK(shaping.value().startsWith(QStringLiteral("transform:")));
    shaping.bar()->buttons()[1]->click();
    CHECK(shaping.value() == QStringLiteral("raw"));
    shaping.bar()->buttons()[2]->click();
    CHECK(rig.editor().save());
    CHECK(rig.actions()[0].shaping == "profile:p1");
}

// from
// tests/test_automation_ui_actions_page.py::test_a_webhook_secret_is_copied_and_dropped_with_an_unsaved_draft
TEST_CASE("test_a_webhook_secret_is_copied_and_dropped_with_an_unsaved_draft",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    fv::ActionsPanel& panel = rig.panel();
    startFrom(panel, "webhook");
    fv::ActionEditor& editor = rig.editor();
    const std::string draftId = fv::qt::toUtf8(editor.actionId());
    editor.copySecret();
    CHECK_FALSE(editor.copySecretButton()->isEnabled());
    CHECK(waitFor([&] { return !rig.copied.isEmpty(); }));
    REQUIRE(rig.secrets.get(draftId).has_value());
    CHECK(rig.copied == QStringList{qs(*rig.secrets.get(draftId))});
    CHECK(editor.message()->text() ==
          QStringLiteral("Signing secret copied. Paste it into your service."));
    CHECK(editor.copySecretButton()->isEnabled());
    CHECK_FALSE(rig.secrets.anyOnGuiThread());
    editor.cancel();
    CHECK(waitFor([&] { return !rig.secrets.get(draftId).has_value(); }));

    startFrom(panel, "webhook");
    editor.urlEdit()->setText(QStringLiteral("https://example.com/hook"));
    const std::string savedId = fv::qt::toUtf8(editor.actionId());
    editor.copySecret();
    CHECK(waitFor([&] { return rig.copied.size() == 2; }));
    CHECK(editor.save());
    pump();
    REQUIRE(rig.secrets.get(savedId).has_value());
    CHECK(qs(*rig.secrets.get(savedId)) == rig.copied.last());
}

// from
// tests/test_automation_ui_actions_page.py::test_a_new_secret_replaces_the_old_one_after_asking
TEST_CASE("test_a_new_secret_replaces_the_old_one_after_asking",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kHook}));
    rig.dialog();
    rig.secrets.put("h1", "whsec_old");
    fv::ActionsPanel& panel = rig.panel();
    panel.edit(QStringLiteral("h1"));
    rig.answer = QMessageBox::Cancel;
    rig.editor().newSecret();
    pump();
    CHECK(rig.secrets.get("h1") == std::optional<std::string>("whsec_old"));
    CHECK(rig.copied.isEmpty());
    rig.answer = QMessageBox::Yes;
    rig.editor().newSecret();
    CHECK(waitFor([&] { return !rig.copied.isEmpty(); }));
    CHECK(rig.copied == QStringList{qs(rig.secrets.get("h1").value_or(""))});
    CHECK(rig.copied[0] != QStringLiteral("whsec_old"));
    CHECK(rig.editor().message()->text() ==
          QStringLiteral("New signing secret copied. Update your service with it."));
    rig.editor().cancel();
    pump();
    // A saved action's secret stays when its edit is cancelled.
    CHECK(qs(rig.secrets.get("h1").value_or("")) == rig.copied[0]);
}

// from tests/test_automation_ui_actions_page.py::test_a_secret_the_store_cant_give_is_explained
TEST_CASE("test_a_secret_the_store_cant_give_is_explained",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.secrets.refuse = "Windows Credential Manager is locked";
    rig.dialog();
    startFrom(rig.panel(), "webhook");
    rig.editor().copySecret();
    CHECK(waitFor([&] { return rig.editor().copySecretButton()->isEnabled(); }));
    CHECK(rig.editor().message()->text() ==
          QStringLiteral("Couldn't get the signing secret: Windows Credential Manager is locked"));
    CHECK(rig.copied.isEmpty());
}

// from
// tests/test_automation_ui_actions_page.py::test_a_plain_http_address_off_this_network_is_flagged
TEST_CASE("test_a_plain_http_address_off_this_network_is_flagged",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    startFrom(rig.panel(), "webhook");
    fv::ActionEditor& editor = rig.editor();
    editor.urlEdit()->setText(QStringLiteral("http://example.com/hook"));
    CHECK(editor.urlNote()->isVisible());
    editor.urlEdit()->setText(QStringLiteral("http://192.168.1.20:5678/hook"));
    CHECK_FALSE(editor.urlNote()->isVisible());
    editor.urlEdit()->setText(QStringLiteral("https://example.com/hook"));
    CHECK_FALSE(editor.urlNote()->isVisible());
}

// from
// tests/test_automation_ui_actions_page.py::test_a_program_needs_its_full_path_and_keeps_arguments_one_per_line
TEST_CASE("test_a_program_needs_its_full_path_and_keeps_arguments_one_per_line",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    startFrom(rig.panel(), "program");
    fv::ActionEditor& editor = rig.editor();
    CHECK_FALSE(editor.save());
    CHECK(editor.message()->text() == QStringLiteral("Choose the program to run."));
    fv::testkit::TempDataRoot folder;
    const QString program = QString::fromStdWString((folder.path() / "journal.exe").wstring());
    fv::action_fields::PathChooser previous = fv::action_fields::chooseProgram();
    fv::action_fields::chooseProgram() = [program](QWidget*, const QString&, const QString&) {
        return program;
    };
    editor.programField()->chooseButton()->click();
    fv::action_fields::chooseProgram() = previous;
    CHECK(editor.programField()->path() == QDir::toNativeSeparators(QDir::cleanPath(program)));
    editor.argsEdit()->setPlainText(QStringLiteral("--append\n\n  --title My day  \n"));
    editor.timeoutSpin()->setValue(45);
    editor.programOutput()->setOutput(QStringLiteral("scratchpad"));
    CHECK(editor.save());
    const Action saved = rig.actions()[0];
    CHECK(saved.param("args") == Json::array({"--append", "--title My day"}));
    CHECK(saved.param("timeout_s") == 45);
    CHECK(saved.param("output") == "scratchpad");
}

// _fill's int(program.param("timeout_s") or 30), over time limits a hand-edited file can hold:
// load_actions keeps only a whole number (_coerce), and the box clamps it to 1 to 300 seconds,
// also one far beyond int's range.
TEST_CASE("a program's time limit shows as OpenWhisper reads it from the file", "[widget]") {
    const auto program = [](const char* id, const Json& timeout) {
        Json params{{"program", "C:\\Tools\\journal.exe"}};
        if (!timeout.is_null()) {
            params["timeout_s"] = timeout;
        }
        return Json{{"id", id}, {"name", id}, {"kind", "program"}, {"params", params}};
    };
    ActionsRig rig(withActions({program("missing", nullptr), program("zero", 0),
                                program("half", 0.5), program("on", true), program("whole", 45),
                                program("negative", -5), program("huge", std::int64_t{1} << 40),
                                program("tiny", -(std::int64_t{1} << 40))}));
    rig.dialog();
    const std::vector<std::pair<const char*, int>> expected{
        {"missing", 30}, {"zero", 30},    {"half", 30},  {"on", 30},
        {"whole", 45},   {"negative", 1}, {"huge", 300}, {"tiny", 1}};
    for (const auto& [id, seconds] : expected) {
        INFO(id);
        rig.panel().edit(QString::fromLatin1(id));
        REQUIRE(rig.editor().actionId() == QString::fromLatin1(id));
        CHECK(rig.editor().timeoutSpin()->value() == seconds);
        rig.editor().cancel();
    }
}

// from tests/test_automation_ui_actions_page.py::test_an_agent_action_keeps_its_agent_choices
TEST_CASE("test_an_agent_action_keeps_its_agent_choices",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    startFrom(rig.panel(), "quick-answer");
    fv::AgentFields& fields = *rig.editor().agentFields();
    CHECK_FALSE(fields.sessionBox()->isVisible());
    CHECK(fields.answerBox()->isVisible());
    CHECK_FALSE(fields.chooser()->choice(QStringLiteral("opencode_cli"))->isEnabled());
    fields.chooser()->choice(QStringLiteral("codex"))->click();
    fields.outputCombo()->setOutput(QStringLiteral("notice"));
    CHECK(rig.editor().save());
    const Action saved = rig.actions()[0];
    CHECK(saved.param("agent") == "codex");
    CHECK(saved.param("mode") == "ask");
    CHECK(saved.param("output") == "notice");
}

// ---- try it ----

// from tests/test_automation_ui_actions_page.py::test_try_it_shows_what_the_runtime_answered
TEST_CASE("test_try_it_shows_what_the_runtime_answered",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    std::vector<std::pair<std::string, std::string>> calls;
    rig.tester().handler = [&calls](const Action& action, const std::string& sample,
                                    std::function<void(ActionResult)> done) {
        calls.emplace_back(action.name, sample);
        done({true, "Opened Claude.", "", false});
    };
    startFrom(rig.panel(), "claude-chat");
    fv::ActionEditor& editor = rig.editor();
    editor.sampleEdit()->setText(QStringLiteral("hello there"));
    editor.tryIt();
    pump();
    CHECK(calls == std::vector<std::pair<std::string, std::string>>{{"Ask Claude", "hello there"}});
    CHECK(editor.tryResult()->text() == QStringLiteral("Opened Claude."));
    CHECK(editor.tryResult()->property("tone").toString() == QStringLiteral("ok"));
    CHECK(editor.tryButton()->isEnabled());
}

// from
// tests/test_automation_ui_actions_page.py::test_try_it_waits_for_an_answer_from_another_thread
TEST_CASE("test_try_it_waits_for_an_answer_from_another_thread",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    std::latch release(1);
    // Joined when the test ends, also when a check failed first (the latch is released by then).
    std::jthread worker;
    rig.tester().handler = [&](const Action&, const std::string&,
                               std::function<void(ActionResult)> done) {
        worker = std::jthread([&release, done = std::move(done)] {
            release.wait();
            done({false, "No app opens claude: links.", "", false});
        });
    };
    startFrom(rig.panel(), "claude-chat");
    fv::ActionEditor& editor = rig.editor();
    editor.tryIt();
    CHECK(editor.tryResult()->text() == QStringLiteral("Trying…"));
    CHECK_FALSE(editor.tryButton()->isEnabled());
    release.count_down();
    CHECK(waitFor([&] {
        return editor.tryResult()->text() == QStringLiteral("No app opens claude: links.");
    }));
    CHECK(editor.tryResult()->property("tone").toString() == QStringLiteral("error"));
    CHECK(editor.tryButton()->isEnabled());
}

// from tests/test_automation_ui_actions_page.py::test_try_it_explains_a_draft_it_cant_run
TEST_CASE("test_try_it_explains_a_draft_it_cant_run", "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    int calls = 0;
    rig.tester().handler = [&calls](const Action&, const std::string&,
                                    std::function<void(ActionResult)>) {
        ++calls;
    };
    startFrom(rig.panel(), "webhook");
    rig.editor().tryIt();
    CHECK(rig.editor().tryResult()->text() == QStringLiteral("Enter the address to send to."));
    CHECK(calls == 0);
}

// from tests/test_automation_ui_actions_page.py::test_try_it_without_the_app_says_so
TEST_CASE("test_try_it_without_the_app_says_so", "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    startFrom(rig.panel(), "copy");
    rig.editor().tryIt();
    pump();
    CHECK(rig.editor().tryResult()->text() ==
          QStringLiteral("Trying an action works while %1 is running.")
              .arg(qs(fv::identity().product)));
}

// ---- drafts ----

// from
// tests/test_automation_ui_actions_page.py::test_an_edited_draft_is_saved_on_leaving_and_an_untouched_one_is_not
TEST_CASE("test_an_edited_draft_is_saved_on_leaving_and_an_untouched_one_is_not",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    startFrom(rig.panel(), "copy");
    dialog.selectDestination(qs(dest::general));
    CHECK(rig.actions().empty());
    dialog.selectDestination(qs(dest::actions));
    rig.editor().nameEdit()->setText(QStringLiteral("Stash it"));
    dialog.selectDestination(qs(dest::general));
    const std::vector<Action> actions = rig.actions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].name == "Stash it");
}

// from tests/test_automation_ui_actions_page.py::test_an_invalid_draft_asks_before_it_is_discarded
TEST_CASE("test_an_invalid_draft_asks_before_it_is_discarded",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    startFrom(rig.panel(), "own-link");
    rig.editor().nameEdit()->setText(QStringLiteral("Search"));
    rig.answer = QMessageBox::Cancel;
    CHECK_FALSE(rig.page().saveDrafts(true));
    REQUIRE_FALSE(rig.questions.empty());
    CHECK(rig.questions.back().second.contains(QStringLiteral("Enter the link to open.")));
    rig.answer = QMessageBox::Discard;
    CHECK(rig.page().saveDrafts(true));
    CHECK(rig.panel().list()->isVisible());
    CHECK(rig.actions().empty());
}

// ---- the agent hotkey card ----

// from tests/test_automation_ui_actions_page.py::test_the_agent_card_sets_up_the_agent_action
TEST_CASE("test_the_agent_card_sets_up_the_agent_action",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    fv::AgentHotkeyCard& card = rig.card();
    CHECK(card.setupButton()->isEnabled());
    CHECK_FALSE(card.fields()->details()->isVisible());
    CHECK(card.fields()->chooser()->current() == QStringLiteral("claude_code"));
    card.fields()->chooser()->choice(QStringLiteral("codex"))->click();
    card.setupButton()->click();
    const auto action = rig.stored("agent");
    REQUIRE(action.has_value());
    CHECK(action->kind == "agent");
    CHECK(action->param("agent") == "codex");
    CHECK(action->param("mode") == "handoff");
    CHECK(action->spoken_names == std::vector<std::string>{"Codex"});
    CHECK(card.fields()->details()->isVisible());
    CHECK(card.shortcutBox()->isVisible());
    CHECK(card.shortcut()->input()->isCapturing());
    card.shortcut()->input()->cancelCapture();
    Q_EMIT card.shortcut()->captured(QStringLiteral("ctrl+alt+a"));
    CHECK(rig.stored("agent")->hotkey == "ctrl+alt+a");
    CHECK(card.message()->text().startsWith(QStringLiteral("Press ")));
    CHECK(rig.changes.contains(QStringLiteral("actions")));
    CHECK(fv::actions_page::rail_value(*rig.settings().snapshot()) == "Agent hotkey set");
    // The agent action has its own card, not one in the list.
    CHECK(rig.panel().list()->cards().empty());
}

// from tests/test_automation_ui_actions_page.py::test_agent_card_choices_save_as_they_change
TEST_CASE("test_agent_card_choices_save_as_they_change",
          "[test_automation_ui_actions_page][widget]") {
    Json agent{{"id", "agent"},
               {"name", "Agent hotkey"},
               {"kind", "agent"},
               {"params", {{"agent", "claude_code"}}},
               {"spoken_names", Json::array({"Claude Code"})},
               {"description", "Coding tasks for Claude Code"}};
    ActionsRig rig(withActions({agent}));
    rig.dialog();
    fv::AgentHotkeyCard& card = rig.card();
    fv::AgentFields& fields = *card.fields();
    fields.modeBar()->buttons()[1]->click();
    CHECK(rig.stored("agent")->param("mode") == "ask");
    CHECK(card.descriptionLabel()->text() ==
          QStringLiteral("Asks Claude Code and shows the answer in the Scratchpad."));
    fields.chooser()->choice(QStringLiteral("codex"))->click();
    Action saved = *rig.stored("agent");
    // The name you say follows the agent, since it was never changed by hand.
    CHECK(saved.param("agent") == "codex");
    CHECK(saved.spoken_names == std::vector<std::string>{"Codex"});
    CHECK(saved.description == "Coding tasks for Codex");
    fields.selectionRow()->switchButton()->click();
    CHECK(rig.stored("agent")->param("include_selection") == true);
    // App awareness is on by default, so there is nothing to point at.
    CHECK_FALSE(fields.selectionNote()->isVisible());
    card.enabledSwitch()->click();
    CHECK(rig.stored("agent")->enabled == false);
}

// from
// tests/test_automation_ui_actions_page.py::test_agent_selection_points_at_app_awareness_when_it_is_off
TEST_CASE("test_agent_selection_points_at_app_awareness_when_it_is_off",
          "[test_automation_ui_actions_page][widget]") {
    Json values = withActions({agentWith({{"agent", "claude_code"}, {"include_selection", true}})});
    values["app_context_enabled"] = false;
    ActionsRig rig(values);
    fv::SettingsDialog& dialog = rig.dialog();
    fv::AgentFields& fields = *rig.card().fields();
    CHECK(fields.selectionNote()->isVisible());
    fields.stylesLink()->click();
    CHECK(dialog.currentDestination() == qs(dest::styles));
}

// from
// tests/test_automation_ui_actions_page.py::test_automatic_folder_choice_saves_and_restores_the_chosen_folder
TEST_CASE("test_automatic_folder_choice_saves_and_restores_the_chosen_folder",
          "[test_automation_ui_actions_page][widget]") {
    const std::string cwd = std::filesystem::current_path().string();
    ActionsRig rig(withActions({agentWith({{"agent", "codex"}, {"folder", cwd}})}));
    rig.dialog();
    fv::AgentHotkeyCard& card = rig.card();
    fv::AgentFields& fields = *card.fields();
    CHECK_FALSE(fields.newFolderRow()->switchButton()->isChecked());
    CHECK(fields.folder()->isVisible());

    fields.newFolderRow()->switchButton()->click();
    Action saved = *rig.stored("agent");
    CHECK(saved.param("new_folder") == true);
    CHECK(saved.param("folder") == cwd);
    CHECK_FALSE(fields.folder()->isVisible());
    CHECK(card.descriptionLabel()->text().contains(QStringLiteral("in a new folder")));
    CHECK(fields.newFolderRow()->detailLabel()->text().contains(QStringLiteral("local project")));

    card.load(*rig.settings().snapshot());
    CHECK(fields.newFolderRow()->switchButton()->isChecked());
    CHECK_FALSE(fields.folder()->isVisible());
    fields.modeBar()->buttons()[1]->click();
    CHECK_FALSE(fields.newFolderRow()->isVisible());
    CHECK(rig.stored("agent")->param("new_folder") == true);
    fields.modeBar()->buttons()[0]->click();
    fields.newFolderRow()->switchButton()->click();
    CHECK(fields.folder()->isVisible());
    CHECK(fields.folder()->path() == qs(cwd));
    CHECK(rig.stored("agent")->param("new_folder") == false);
}

// from tests/test_automation_ui_actions_page.py::test_full_editor_keeps_the_automatic_folder_choice
TEST_CASE("test_full_editor_keeps_the_automatic_folder_choice",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig(withActions({agentWith({{"agent", "codex"}, {"new_folder", true}})}));
    rig.dialog();
    rig.card().moreButton()->click();
    fv::AgentFields& fields = *rig.editor().agentFields();
    CHECK(fields.newFolderRow()->switchButton()->isChecked());
    CHECK_FALSE(fields.folder()->isVisible());
    fields.surfaceBar()->buttons()[2]->click();
    CHECK_FALSE(
        fields.newFolderRow()->detailLabel()->text().contains(QStringLiteral("local project")));
    rig.editor().saveButton()->click();
    const Action saved = *rig.stored("agent");
    CHECK(saved.param("new_folder") == true);
    CHECK(saved.param("surface") == "terminal");
}

// from
// tests/test_automation_ui_actions_page.py::test_agent_card_looks_for_agents_then_says_none_were_found
TEST_CASE("test_agent_card_looks_for_agents_then_says_none_were_found",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    // Nothing was scanned, and the scan finds nothing, the way an empty computer does.
    rig.forgetScan();
    rig.dialog();
    fv::AgentHotkeyCard& card = rig.card();
    CHECK(waitFor([&] { return card.status()->text() == qs(fv::action_agent_card::NONE_FOUND); }));
    CHECK_FALSE(card.setupButton()->isEnabled());
    CHECK(card.lookAgain()->isVisible());
    rig.setFoundOnNextScan({{"claude_code", true}});
    card.lookAgain()->click();
    CHECK(card.status()->text() == qs(fv::action_agent_card::LOOKING));
    CHECK(waitFor([&] { return card.setupButton()->isEnabled(); }));
    CHECK(card.fields()->chooser()->choice(QStringLiteral("claude_code"))->detailLabel()->text() ==
          QStringLiteral("Found"));
    CHECK_FALSE(card.statusRow()->isVisible());
}

// from tests/test_automation_ui_actions_page.py::test_opencode_sessions_open_in_a_terminal
TEST_CASE("test_opencode_sessions_open_in_a_terminal",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig(withActions({agentWith({{"agent", "claude_code"}, {"surface", "app"}})}));
    rig.setFound({{"claude_code", true}, {"codex", false}, {"opencode_cli", true}});
    rig.dialog();
    fv::AgentFields& fields = *rig.card().fields();
    QAbstractButton* app = fields.surfaceBar()->buttons()[1];
    CHECK(app->isEnabled());
    CHECK(app->isChecked());
    fields.chooser()->choice(QStringLiteral("opencode_cli"))->click();
    const Action saved = *rig.stored("agent");
    CHECK(saved.param("agent") == "opencode_cli");
    CHECK(saved.param("surface") == "auto");
    CHECK_FALSE(app->isEnabled());
    CHECK(fields.surfaceHint()->text() ==
          QStringLiteral("OpenCode has no app, so it opens in a new terminal and starts on your "
                         "words right away."));
}

// from
// tests/test_automation_ui_actions_page.py::test_more_options_opens_the_full_editor_on_the_agent_action
TEST_CASE("test_more_options_opens_the_full_editor_on_the_agent_action",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig(withActions({agentWith({{"agent", "claude_code"}})}));
    rig.dialog();
    rig.card().moreButton()->click();
    fv::ActionEditor& editor = rig.editor();
    CHECK(editor.isVisible());
    CHECK(editor.actionId() == QStringLiteral("agent"));
    editor.descriptionEdit()->setText(QStringLiteral("Fixes in this repo"));
    CHECK(editor.save());
    CHECK(rig.stored("agent")->description == "Fixes in this repo");
    REQUIRE(rig.card().action().has_value());
    CHECK(rig.card().action()->description == "Fixes in this repo");
}

// ---- route by voice ----

// from tests/test_automation_ui_actions_page.py::test_routing_switches_save_and_gate_each_other
TEST_CASE("test_routing_switches_save_and_gate_each_other",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    fv::SettingTile& routing = *rig.page().routingTile();
    fv::SettingTile& onDictation = *rig.page().routingDictationTile();
    CHECK_FALSE(onDictation.isEnabled());
    routing.checkbox()->click();
    CHECK(rig.saved("voice_routing_enabled") == true);
    CHECK(onDictation.isEnabled());
    onDictation.checkbox()->click();
    CHECK(rig.saved("voice_routing_on_dictation") == true);
    routing.checkbox()->click();
    CHECK(rig.saved("voice_routing_enabled") == false);
    CHECK_FALSE(onDictation.isEnabled());
}

// from
// tests/test_automation_ui_actions_page.py::test_routing_status_shows_routings_own_words_and_links_to_its_fix
TEST_CASE("test_routing_status_shows_routings_own_words_and_links_to_its_fix",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.setStatus(RoutingStatus{false, false, "Route by voice is off.", ""});
    fv::SettingsDialog& dialog = rig.dialog();
    fv::ActionsPage& page = rig.page();
    fv::StatusLine& status = *page.routingStatusLine();
    CHECK(waitFor([&] { return statusText(page) == QStringLiteral("Route by voice is off."); }));
    CHECK(status.state() == QStringLiteral("off"));
    CHECK_FALSE(status.link()->isVisible());
    CHECK(page.routingPrivacy()->isVisible());
    rig.setStatus(RoutingStatus{true, false, "Turn on Fast judgments to route by voice.",
                                std::string(fv::automation::FIX_FAST_JUDGMENTS)});
    page.routingTile()->checkbox()->click();
    CHECK(waitFor([&] {
        return statusText(page) == QStringLiteral("Turn on Fast judgments to route by voice.");
    }));
    CHECK(status.state() == QStringLiteral("attention"));
    CHECK(status.link()->isVisible());
    status.link()->click();
    CHECK(dialog.currentDestination() == qs(dest::meeting_fast));
    rig.setStatus(RoutingStatus{
        true, true, "Ready. Words that name an action go to TypeSafe to choose it.", ""});
    dialog.selectDestination(qs(dest::actions));
    page.refresh();
    CHECK(waitFor([&] { return status.state() == QStringLiteral("ready"); }));
    CHECK_FALSE(status.link()->isVisible());
    CHECK(statusText(page) ==
          QStringLiteral("Ready. Words that name an action go to TypeSafe to choose it."));
    // The ready message carries the privacy sentence itself.
    CHECK_FALSE(page.routingPrivacy()->isVisible());
}

// from
// tests/test_automation_ui_actions_page.py::test_the_route_shortcut_saves_through_the_standard_hotkey_path
TEST_CASE("test_the_route_shortcut_saves_through_the_standard_hotkey_path",
          "[test_automation_ui_actions_page][widget]") {
    ActionsRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    std::vector<std::pair<QString, QString>> spied;
    fv::RouteShortcutField& field = *rig.page().routeShortcut();
    // dialog.set_standard_hotkey wrapped by a spy: the rig's path, recorded.
    dialog.setStandardHotkeyHandler(
        [&rig, &spied, &dialog](const QString& action, const QString& hotkey) {
            spied.emplace_back(action, hotkey);
            return rig.standardHotkey(dialog, action, hotkey);
        });
    Q_EMIT field.input()->captured(QStringLiteral("ctrl+alt+v"));
    CHECK(spied == std::vector<std::pair<QString, QString>>{
                       {QStringLiteral("route_record"), QStringLiteral("ctrl+alt+v")}});
    CHECK(fv::load_hotkey_settings(rig.settings())["route_record"] == "ctrl+alt+v");
    CHECK(field.input()->hotkey() == QStringLiteral("ctrl+alt+v"));
    Q_EMIT field.input()->captured(QStringLiteral("ctrl+alt+m"));
    CHECK(field.message()->text() ==
          QStringLiteral("That shortcut is already used by Minimize to tray."));
    CHECK(fv::load_hotkey_settings(rig.settings())["route_record"] == "ctrl+alt+v");
    field.input()->beginCapture();
    field.input()->cancelCapture();
    CHECK(rig.captures == std::vector<bool>{true, false});
}

// ---- test_automation_fix_ui_actions_page.py ----
// ---- 1. the agent action's full editor and the Agent hotkey card ----

// from
// tests/test_automation_fix_ui_actions_page.py::test_the_agent_editor_follows_the_card_while_it_has_no_edits
TEST_CASE("test_the_agent_editor_follows_the_card_while_it_has_no_edits",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAgent}));
    rig.dialog();
    fv::AgentHotkeyCard& card = rig.card();
    fv::ActionEditor& editor = rig.editor();
    card.moreButton()->click();
    CHECK(editor.actionId() == QStringLiteral("agent"));
    card.enabledSwitch()->click();
    card.fields()->chooser()->choice(QStringLiteral("codex"))->click();
    CHECK(editor.agentFields()->chooser()->current() == QStringLiteral("codex"));
    CHECK(editor.descriptionEdit()->text() == QStringLiteral("Coding tasks for Codex"));
    CHECK_FALSE(editor.hasUnsavedChanges());

    editor.descriptionEdit()->setText(QStringLiteral("Fixes in this repo"));
    CHECK(editor.save());
    const Action saved = *rig.stored("agent");
    CHECK(saved.enabled == false);
    CHECK(saved.param("agent") == "codex");
    CHECK(saved.description == "Fixes in this repo");
    CHECK_FALSE(card.enabledSwitch()->isChecked());
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_the_agent_editor_keeps_its_edits_when_the_card_changes
TEST_CASE("test_the_agent_editor_keeps_its_edits_when_the_card_changes",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAgent}));
    rig.dialog();
    fv::AgentHotkeyCard& card = rig.card();
    fv::ActionEditor& editor = rig.editor();
    card.moreButton()->click();
    editor.descriptionEdit()->setText(QStringLiteral("My words"));
    card.enabledSwitch()->click();
    card.fields()->modeBar()->buttons()[1]->click();
    // The card's change shows in the editor, and your edit is still there.
    CHECK(editor.agentFields()->modeBar()->currentIndex() == 1);
    CHECK(editor.descriptionEdit()->text() == QStringLiteral("My words"));
    CHECK(editor.hasUnsavedChanges());
    CHECK(editor.save());
    const Action saved = *rig.stored("agent");
    CHECK(saved.enabled == false);
    CHECK(saved.param("mode") == "ask");
    CHECK(saved.description == "My words");
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_saving_an_edit_keeps_what_changed_since_it_was_opened
TEST_CASE("test_saving_an_edit_keeps_what_changed_since_it_was_opened",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAskClaude}));
    rig.dialog();
    rig.panel().edit(QStringLiteral("a1"));
    rig.editor().nameEdit()->setText(QStringLiteral("Claude chat"));
    changeStored(rig, "a1", [](Action& action) {
        action.enabled = false;
        action.hotkey = "ctrl+alt+j";
    });
    CHECK(rig.editor().save());
    const Action saved = *rig.stored("a1");
    CHECK(saved.name == "Claude chat");
    CHECK(saved.enabled == false);
    CHECK(saved.hotkey == "ctrl+alt+j");
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_removing_the_agent_hotkey_closes_its_editor_for_good
TEST_CASE("test_removing_the_agent_hotkey_closes_its_editor_for_good",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAgent}));
    rig.dialog();
    fv::AgentHotkeyCard& card = rig.card();
    fv::ActionsPanel& panel = rig.panel();
    card.moreButton()->click();
    rig.editor().descriptionEdit()->setText(QStringLiteral("edit before remove"));
    rig.answer = QMessageBox::Yes;
    card.remove();
    CHECK(panel.modeName() == QStringLiteral("list"));
    CHECK_FALSE(rig.editor().isVisible());
    CHECK(panel.notice()->text() == QStringLiteral("Agent hotkey was removed."));
    // Leaving the page has no draft left to save, so nothing comes back.
    CHECK(rig.page().saveDrafts(true));
    CHECK_FALSE(rig.stored("agent").has_value());
    CHECK_FALSE(card.action().has_value());
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_an_edit_of_an_action_removed_elsewhere_isnt_saved_back
TEST_CASE("test_an_edit_of_an_action_removed_elsewhere_isnt_saved_back",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAskClaude}));
    rig.dialog();
    fv::ActionEditor& editor = rig.editor();
    rig.panel().edit(QStringLiteral("a1"));
    editor.nameEdit()->setText(QStringLiteral("Claude chat"));
    rig.settings().mutate([](Json& settings) { fv::automation::delete_action(settings, "a1"); });
    CHECK_FALSE(editor.save());
    CHECK_FALSE(rig.stored("a1").has_value());
    CHECK(editor.message()->text() ==
          QStringLiteral(
              "Ask Claude no longer exists, so there's nothing to save these changes to."));
}

// ---- 2. "Customize" while another action is being edited ----

// from
// tests/test_automation_fix_ui_actions_page.py::test_more_options_asks_before_leaving_an_unsaved_edit
TEST_CASE("test_more_options_asks_before_leaving_an_unsaved_edit",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAgent, kAskClaude}));
    rig.dialog();
    fv::AgentHotkeyCard& card = rig.card();
    fv::ActionsPanel& panel = rig.panel();
    fv::ActionEditor& editor = rig.editor();
    panel.edit(QStringLiteral("a1"));
    editor.nameEdit()->setText(QStringLiteral("Claude chat"));

    rig.answer = QMessageBox::Cancel;
    card.moreButton()->click();
    CHECK(rig.questions == std::vector<std::pair<QString, QString>>{
                               {QStringLiteral("Unsaved action"),
                                QStringLiteral("Save your changes to Claude chat?")}});
    CHECK(editor.actionId() == QStringLiteral("a1"));
    CHECK(editor.nameEdit()->text() == QStringLiteral("Claude chat"));

    rig.answer = QMessageBox::Discard;
    card.moreButton()->click();
    CHECK(editor.actionId() == QStringLiteral("agent"));
    CHECK(rig.stored("a1")->name == "Ask Claude");

    panel.edit(QStringLiteral("a1"));
    editor.nameEdit()->setText(QStringLiteral("Claude chat"));
    rig.answer = QMessageBox::Save;
    card.moreButton()->click();
    CHECK(rig.stored("a1")->name == "Claude chat");
    CHECK(editor.actionId() == QStringLiteral("agent"));
    CHECK(panel.modeName() == QStringLiteral("editor"));
}

// from tests/test_automation_fix_ui_actions_page.py::test_more_options_keeps_an_edit_it_cant_save
TEST_CASE("test_more_options_keeps_an_edit_it_cant_save",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAgent, kAskClaude}));
    rig.dialog();
    fv::ActionsPanel& panel = rig.panel();
    panel.edit(QStringLiteral("a1"));
    rig.editor().templateEdit()->setText(QString());
    rig.answer = QMessageBox::Save;
    rig.card().moreButton()->click();
    CHECK(rig.editor().actionId() == QStringLiteral("a1"));
    CHECK(rig.editor().message()->text() == QStringLiteral("Enter the link to open."));
    CHECK(rig.stored("a1")->param("template") == "claude://claude.ai/new?q={text}");
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_more_options_on_the_open_agent_editor_keeps_its_edits
TEST_CASE("test_more_options_on_the_open_agent_editor_keeps_its_edits",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAgent}));
    rig.dialog();
    rig.card().moreButton()->click();
    rig.editor().descriptionEdit()->setText(QStringLiteral("Mine"));
    rig.card().moreButton()->click();
    CHECK(rig.questions.empty());
    CHECK(rig.editor().descriptionEdit()->text() == QStringLiteral("Mine"));
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_replacing_a_new_webhook_drops_the_secret_it_made
TEST_CASE("test_replacing_a_new_webhook_drops_the_secret_it_made",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAgent}));
    rig.dialog();
    fv::ActionsPanel& panel = rig.panel();
    startFrom(panel, "webhook");
    const std::string draftId = fv::qt::toUtf8(rig.editor().actionId());
    rig.editor().copySecret();
    CHECK(waitFor([&] { return !rig.copied.isEmpty(); }));
    CHECK(rig.secrets.get(draftId).has_value());
    // Copying a secret changes nothing in the draft, so nothing is asked.
    rig.card().moreButton()->click();
    CHECK(rig.questions.empty());
    CHECK(rig.editor().actionId() == QStringLiteral("agent"));
    CHECK(waitFor([&] { return !rig.secrets.get(draftId).has_value(); }));
}

// ---- 3. Route by voice's status keeps up ----

namespace {

// Route by voice on, and listening to normal dictations, so it has a way to start.
Json routingOn(Json values = Json::object()) {
    values["voice_routing_enabled"] = true;
    values["voice_routing_on_dictation"] = true;
    return values;
}

} // namespace

// from tests/test_automation_fix_ui_actions_page.py::test_route_by_voice_status_follows_the_actions
TEST_CASE("test_route_by_voice_status_follows_the_actions",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(routingOn(Json{{"typesafe_enabled", true}}));
    rig.dialog();
    fv::ActionsPage& page = rig.page();
    CHECK(waitFor([&] { return statusText(page) == qs(rig.routingWords()); }));
    const QString noActions = statusText(page);
    startFrom(rig.panel(), "claude-chat");
    CHECK(rig.editor().save());
    CHECK(waitFor([&] { return statusText(page) == qs(rig.routingWords()); }));
    CHECK(statusText(page) != noActions);
    const QString ready = statusText(page);
    rig.panel().setActionEnabled(qs(rig.panel().list()->cards()[0]->action().id), false);
    CHECK(waitFor([&] { return statusText(page) == qs(rig.routingWords()); }));
    CHECK(statusText(page) != noActions);
    CHECK(statusText(page) != ready);
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_route_by_voice_status_follows_the_agent_card
TEST_CASE("test_route_by_voice_status_follows_the_agent_card",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(routingOn(Json{{"typesafe_enabled", true}}));
    rig.dialog();
    fv::ActionsPage& page = rig.page();
    CHECK(waitFor([&] { return statusText(page) == qs(rig.routingWords()); }));
    const QString noActions = statusText(page);
    rig.card().setUp();
    rig.card().cancelCapture();
    CHECK(waitFor([&] { return statusText(page) == qs(rig.routingWords()); }));
    CHECK(statusText(page) != noActions);
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_route_by_voice_status_is_read_again_when_the_page_returns
TEST_CASE("test_route_by_voice_status_is_read_again_when_the_page_returns",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(routingOn(withActions({kAskClaude})));
    fv::SettingsDialog& dialog = rig.dialog();
    fv::ActionsPage& page = rig.page();
    CHECK(
        waitFor([&] { return page.routingStatusLine()->state() == QStringLiteral("attention"); }));
    const QString needsFast = statusText(page);
    dialog.selectDestination(qs(dest::general));
    // Turned on elsewhere, as the Fast judgments link leads you to.
    rig.settings().save_setting("typesafe_enabled", true);
    dialog.selectDestination(qs(dest::actions));
    pump();
    CHECK(waitFor([&] { return page.routingStatusLine()->state() == QStringLiteral("ready"); }));
    CHECK(statusText(page) == qs(rig.routingWords()));
    CHECK(statusText(page) != needsFast);
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_route_by_voice_status_follows_its_shortcut
TEST_CASE("test_route_by_voice_status_follows_its_shortcut",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(Json{{"voice_routing_enabled", true}});
    // Whether anything starts Route by voice depends on its shortcut.
    rig.setStatusFunction([](const Json& settings) {
        const auto hotkeys = settings.find("hotkeys");
        if (hotkeys != settings.end() && hotkeys->is_object() &&
            !text(hotkeys->value("route_record", Json())).empty()) {
            return RoutingStatus{true, true, "Route by voice is on.", ""};
        }
        return RoutingStatus{true, false, "Set a Route by voice shortcut below.",
                             std::string(fv::automation::FIX_TRIGGER)};
    });
    rig.dialog();
    fv::ActionsPage& page = rig.page();
    fv::StatusLine& line = *page.routingStatusLine();
    CHECK(waitFor([&] {
        return statusText(page) == QStringLiteral("Set a Route by voice shortcut below.");
    }));
    Q_EMIT page.routeShortcut()->input()->captured(QStringLiteral("ctrl+alt+v"));
    CHECK(waitFor([&] { return line.state() == QStringLiteral("ready"); }));
    CHECK(statusText(page) == QStringLiteral("Route by voice is on."));
    Q_EMIT page.routeShortcut()->input()->captured(QString());
    CHECK(waitFor([&] {
        return statusText(page) == QStringLiteral("Set a Route by voice shortcut below.");
    }));
}

// ---- Try it runs end with Settings ----

// from
// tests/test_automation_fix_ui_actions_page.py::test_the_app_passes_settings_a_way_to_cancel_try_runs
// The app's way is the context's ActionTester (UIController._cancel_try_runs in Python): without
// one there is nothing to cancel; with one, the page's cancel reaches it once.
TEST_CASE("test_the_app_passes_settings_a_way_to_cancel_try_runs",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    rig.page().cancelTryRuns(); // Not wired yet: nothing to cancel.
    FakeTester& tester = rig.tester();
    rig.page().cancelTryRuns();
    CHECK(tester.cancels == 1);
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_closing_settings_cancels_try_runs_still_going
TEST_CASE("test_closing_settings_cancels_try_runs_still_going",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(withActions({agentWith({{"agent", "claude_code"}, {"mode", "ask"}})}));
    fv::SettingsDialog& dialog = rig.dialog();
    std::vector<std::function<void(ActionResult)>> running;
    FakeTester& tester = rig.tester();
    tester.handler = [&running](const Action&, const std::string&,
                                std::function<void(ActionResult)> done) {
        running.push_back(std::move(done));
    };
    tester.cancel = [&running] {
        // As ActionRuntime.cancel(tests_only=True) does: each run ends "Canceled".
        int stopped = 0;
        while (!running.empty()) {
            auto done = std::move(running.back());
            running.pop_back();
            done({false, "Canceled", "", false});
            ++stopped;
        }
        return stopped;
    };
    fv::AgentHotkeyCard& card = rig.card();
    card.tryButton()->click();
    CHECK_FALSE(card.tryButton()->isEnabled());
    CHECK_FALSE(running.empty());
    dialog.close();
    CHECK(tester.cancels == 1);
    pump();
    CHECK(card.tryButton()->isEnabled());
    CHECK(card.message()->text() == QStringLiteral("Canceled"));
}

// from tests/test_automation_fix_ui_actions_page.py::test_closing_settings_without_the_app_is_quiet
TEST_CASE("test_closing_settings_without_the_app_is_quiet",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    CHECK(rig.context().find<fv::automation::ActionTester>() == nullptr);
    dialog.close();
    pump();
}

// ---- 4/5. Route by voice's status, links and privacy line ----

// from
// tests/test_automation_fix_ui_actions_page.py::test_the_fast_judgments_link_shows_only_when_they_are_the_fix
TEST_CASE("test_the_fast_judgments_link_shows_only_when_they_are_the_fix",
          "[test_automation_fix_ui_actions_page][widget]") {
    for (const auto& [fix, linked] : std::vector<std::pair<std::string, bool>>{
             {"fast_judgments", true}, {"actions", false}, {"trigger", false}, {"", false}}) {
        INFO(fix);
        ActionsRig rig(Json{{"voice_routing_enabled", true}});
        rig.setStatus(RoutingStatus{true, false, "Route by voice needs something.", fix});
        rig.dialog();
        fv::StatusLine& status = *rig.page().routingStatusLine();
        CHECK(waitFor([&] { return status.state() == QStringLiteral("attention"); }));
        CHECK(status.link()->isVisible() == linked);
    }
}

// from tests/test_automation_fix_ui_actions_page.py::test_route_by_voice_off_has_no_link
TEST_CASE("test_route_by_voice_off_has_no_link", "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.setStatus(RoutingStatus{false, false, "Route by voice is off.", ""});
    rig.dialog();
    fv::StatusLine& status = *rig.page().routingStatusLine();
    CHECK(waitFor(
        [&] { return status.message()->text() == QStringLiteral("Route by voice is off."); }));
    CHECK(status.state() == QStringLiteral("off"));
    CHECK_FALSE(status.link()->isVisible());
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_the_privacy_line_says_what_each_way_of_routing_sends
TEST_CASE("test_the_privacy_line_says_what_each_way_of_routing_sends",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(Json{{"voice_routing_enabled", true}});
    rig.setStatus(RoutingStatus{true, false, "Route by voice needs Fast judgments.",
                                std::string(fv::automation::FIX_FAST_JUDGMENTS)});
    rig.dialog();
    fv::ActionsPage& page = rig.page();
    fv::WrappedLabel& privacy = *page.routingPrivacy();
    CHECK(
        waitFor([&] { return page.routingStatusLine()->state() == QStringLiteral("attention"); }));
    CHECK(privacy.isVisible());
    CHECK(privacy.text() == QStringLiteral("Everything you record with the Route by voice shortcut "
                                           "is given to the Fast judgments judge to choose the "
                                           "action."));
    page.routingDictationTile()->checkbox()->click();
    CHECK(privacy.text() ==
          QStringLiteral("Everything you record with the Route by voice shortcut, and normal "
                         "dictations that start with an action's name, are given to the Fast "
                         "judgments judge to choose the action."));
    // Ready, routing's own words say what is sent.
    rig.setStatus(RoutingStatus{true, true, "Route by voice is on.", ""});
    page.refresh();
    CHECK(waitFor([&] { return !privacy.isVisible(); }));
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_route_by_voice_has_no_caption_repeating_its_tile
TEST_CASE("test_route_by_voice_has_no_caption_repeating_its_tile",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    QStringList captions;
    for (const QLabel* label :
         dialog.page(dest::actions)
             ->findChildren<QLabel*>(QStringLiteral("settingsTileGroupTitle"))) {
        if (label->isVisible()) {
            captions << label->text();
        }
    }
    CHECK_FALSE(captions.contains(QStringLiteral("Route by voice")));
    CHECK(rig.page().routingTile()->titleLabel()->text() == QStringLiteral("Route by voice"));
}

// ---- 6/7. What "Try it" shows ----

// from tests/test_automation_fix_ui_actions_page.py::test_the_agent_card_shows_a_quick_answer
TEST_CASE("test_the_agent_card_shows_a_quick_answer",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(withActions({agentWith({{"agent", "claude_code"}, {"mode", "ask"}})}));
    rig.dialog();
    rig.tester().handler = [](const Action&, const std::string&,
                              std::function<void(ActionResult)> done) {
        done({true, "Asked Claude Code", "Hello! Nice to meet you.", false});
    };
    fv::AgentHotkeyCard& card = rig.card();
    card.tryButton()->click();
    pump();
    CHECK(card.message()->text() == QStringLiteral("Asked Claude Code\nHello! Nice to meet you."));
    CHECK(card.message()->property("tone").toString() == QStringLiteral("ok"));
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_try_it_shows_only_the_start_of_a_long_output
TEST_CASE("test_try_it_shows_only_the_start_of_a_long_output",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    std::string lines;
    for (int i = 0; i < 4000; ++i) {
        lines += "line of output\n";
    }
    std::vector<std::string> outputs{lines, std::string(5000, 'x')};
    rig.tester().handler = [&outputs](const Action&, const std::string&,
                                      std::function<void(ActionResult)> done) {
        std::string output = outputs.front();
        outputs.erase(outputs.begin());
        done({true, "", output, false});
    };
    startFrom(rig.panel(), "copy");
    fv::ActionEditor& editor = rig.editor();
    editor.tryIt();
    pump();
    QStringList six;
    for (int i = 0; i < 6; ++i) {
        six << QStringLiteral("line of output");
    }
    CHECK(editor.tryResult()->text() ==
          QStringLiteral("It worked.\n") + six.join(QLatin1Char('\n')) + QStringLiteral("…"));
    editor.tryIt();
    pump();
    CHECK(editor.tryResult()->text() ==
          QStringLiteral("It worked.\n") + QString(500, QLatin1Char('x')) + QStringLiteral("…"));
}

// from tests/test_automation_fix_ui_actions_page.py::test_shortened_keeps_what_fits_as_it_is
// The two cases, then every shortened() vector of action_copy (M5.3's golden), each through the
// editor's result box as a Try it output.
TEST_CASE("test_shortened_keeps_what_fits_as_it_is",
          "[test_automation_fix_ui_actions_page][widget]") {
    namespace ac = fv::automation::action_copy;
    CHECK(ac::shortened("  one\r\ntwo  ") == "one\ntwo");
    CHECK(ac::shortened("0\n1\n2\n3\n4\n5\n6") == "0\n1\n2\n3\n4\n5…");

    ActionsRig rig;
    rig.dialog();
    std::string output;
    rig.tester().handler = [&output](const Action&, const std::string&,
                                     std::function<void(ActionResult)> done) {
        done({true, "", output, false});
    };
    startFrom(rig.panel(), "copy");
    fv::ActionEditor& editor = rig.editor();
    // Each vector is a section of its own (for_each fails when there are none).
    fv::golden::for_each("ui_qt.widgets.action_copy", "shortened", [&](const fv::golden::Case& c) {
        // A runtime's result carries text; shortened(None) is not something it can show.
        const auto input = c.arg<std::optional<std::string>>(0);
        if (!input) {
            return;
        }
        output = *input;
        editor.tryIt();
        pump(2);
        const std::string expected =
            "It worked." + (output.empty() ? std::string() : "\n" + c.result<std::string>());
        CHECK(fv::qt::toUtf8(editor.tryResult()->text()) == expected);
    });
}

// ---- 8. An editor opens with its top in view ----

// from
// tests/test_automation_fix_ui_actions_page.py::test_an_editor_opens_with_its_name_field_in_view
TEST_CASE("test_an_editor_opens_with_its_name_field_in_view",
          "[test_automation_fix_ui_actions_page][widget]") {
    for (const char* opened : {"more options", "preset"}) {
        INFO(opened);
        ActionsRig rig(withActions({kAgent, kAskClaude}));
        fv::SettingsDialog& dialog = rig.dialog(true, QSize(1100, 640));
        fv::ActionsPanel& panel = rig.panel();
        if (std::string(opened) == "more options") {
            rig.card().moreButton()->click();
        } else {
            panel.addAction();
            pump();
            panel.startFromPreset(galleryPreset(panel, "webhook"));
        }
        pump(12);
        fv::ActionEditor& editor = rig.editor();
        QScrollArea* area = dialog.pageScroll(dest::actions);
        REQUIRE(area != nullptr);
        // Too tall to show whole.
        CHECK(editor.height() > area->viewport()->height());
        const auto inView = [area](QWidget* widget) {
            const int top = widget->mapTo(area->viewport(), QPoint(0, 0)).y();
            return 0 <= top && top + widget->height() <= area->viewport()->height();
        };
        CHECK(inView(editor.titleLabel()));
        CHECK(inView(editor.nameEdit()));
    }
}

// ---- 9. The working folder is for sessions only ----

// from
// tests/test_automation_fix_ui_actions_page.py::test_the_working_folder_shows_only_for_a_session
TEST_CASE("test_the_working_folder_shows_only_for_a_session",
          "[test_automation_fix_ui_actions_page][widget]") {
    const std::string cwd = std::filesystem::current_path().string();
    ActionsRig rig(withActions({agentWith({{"agent", "claude_code"}, {"folder", cwd}})}));
    rig.dialog();
    fv::AgentFields& fields = *rig.card().fields();
    CHECK(fields.folder()->isVisible());
    fields.modeBar()->buttons()[1]->click();
    CHECK_FALSE(fields.folder()->isVisible());
    // Kept for when you switch back to a session.
    CHECK(rig.stored("agent")->param("folder") == cwd);
    startFrom(rig.panel(), "quick-answer");
    CHECK_FALSE(rig.editor().agentFields()->folder()->isVisible());
}

// ---- 14. Where a session opens says whether the agent waits ----

// from
// tests/test_automation_fix_ui_actions_page.py::test_opens_in_says_a_terminal_starts_right_away_and_an_app_waits
TEST_CASE("test_opens_in_says_a_terminal_starts_right_away_and_an_app_waits",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAgent}));
    rig.dialog();
    fv::AgentFields& fields = *rig.card().fields();
    CHECK(fields.modeHint()->text() ==
          QStringLiteral("Claude Code opens in a new session with your words."));
    QStringList hints;
    for (QAbstractButton* button : fields.surfaceBar()->buttons()) {
        button->click();
        hints << fields.surfaceHint()->text();
    }
    CHECK(hints == QStringList{
                       QStringLiteral("The agent's app when it's installed, with your words "
                                      "filled in for you to send; otherwise a new terminal, where "
                                      "the agent starts on them right away."),
                       QStringLiteral("The agent's app opens with your words filled in, ready for "
                                      "you to send."),
                       QStringLiteral("A new terminal opens, and the agent starts on your words "
                                      "right away."),
                   });
}

// ---- 10. Tab goes through the page as it reads ----

namespace {

// The next `count` widgets Tab would reach on `page` after `start`.
std::vector<QWidget*> tabOrder(QWidget* start, QWidget* page, std::size_t count) {
    std::vector<QWidget*> order;
    QWidget* widget = start;
    while (order.size() < count) {
        widget = widget->nextInFocusChain();
        if (widget == start) {
            break;
        }
        if (page->isAncestorOf(widget) && widget->isVisible() && widget->isEnabled() &&
            (widget->focusPolicy() & Qt::TabFocus) != 0) {
            order.push_back(widget);
        }
    }
    return order;
}

} // namespace

// from
// tests/test_automation_fix_ui_actions_page.py::test_tab_reaches_the_action_cards_where_they_sit
TEST_CASE("test_tab_reaches_the_action_cards_where_they_sit",
          "[test_automation_fix_ui_actions_page][widget]") {
    const Json hook{{"id", "h1"},
                    {"name", "Home Assistant"},
                    {"kind", "webhook"},
                    {"params", {{"url", "https://example.com/hook"}}}};
    ActionsRig rig(withActions({kAskClaude, hook}));
    fv::SettingsDialog& dialog = rig.dialog();
    fv::ActionsPanel& panel = rig.panel();
    const auto expected = [&] {
        std::vector<QWidget*> widgets;
        for (const char* id : {"a1", "h1"}) {
            fv::ActionCard* card = panel.list()->card(QString::fromLatin1(id));
            widgets.push_back(card->switchButton());
            widgets.push_back(card->editLink());
            widgets.push_back(card->deleteLink());
        }
        widgets.push_back(rig.page().routingTile()->checkbox());
        return widgets;
    };
    QWidget* page = dialog.page(dest::actions);
    CHECK(tabOrder(panel.addButton(), page, 7) == expected());
    // Cards made again, after a change, still come where they sit.
    fv::ActionCard* before = panel.list()->card(QStringLiteral("h1"));
    panel.setActionEnabled(QStringLiteral("h1"), false);
    panel.refresh();
    pump();
    CHECK(panel.list()->card(QStringLiteral("h1")) != before);
    CHECK(tabOrder(panel.addButton(), page, 7) == expected());
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_reading_the_same_actions_again_keeps_the_cards
TEST_CASE("test_reading_the_same_actions_again_keeps_the_cards",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAskClaude}));
    rig.dialog();
    fv::ActionsPanel& panel = rig.panel();
    fv::ActionCard* card = panel.list()->card(QStringLiteral("a1"));
    panel.refresh();
    rig.page().refresh();
    CHECK(panel.list()->card(QStringLiteral("a1")) == card);
}

// ---- 11. Settings search finds settings, not the Add action gallery ----

// from
// tests/test_automation_fix_ui_actions_page.py::test_search_leaves_out_tiles_in_a_part_marked_as_choices
// A page of the window's own over build_index's three parts: shown tiles, a folded-away part and
// a part marked as choices.
TEST_CASE("test_search_leaves_out_tiles_in_a_part_marked_as_choices",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig;
    fv::SettingsPageSpec spec;
    spec.destination = dest::dictionary;
    spec.title = QStringLiteral("Page");
    spec.create = [](QWidget* parent) -> QWidget* {
        auto* page = new QWidget(parent);
        auto* folded = new QWidget;
        auto* picker = new QWidget;
        picker->setProperty("settingsSearch", false);
        auto* pageLayout = new QVBoxLayout(page);
        auto* foldedLayout = new QVBoxLayout(folded);
        auto* pickerLayout = new QVBoxLayout(picker);
        const QIcon icon;
        pageLayout->addWidget(new fv::InfoTile(QStringLiteral("Shown"), QString(), icon));
        foldedLayout->addWidget(new fv::InfoTile(QStringLiteral("Folded away"), QString(), icon));
        pickerLayout->addWidget(new fv::InfoTile(QStringLiteral("A choice"), QString(), icon));
        pageLayout->addWidget(folded);
        pageLayout->addWidget(picker);
        // A collapsed section or another tab: still settings, still found.
        folded->hide();
        return page;
    };
    rig.context().settingsPages().addPage(std::move(spec));
    fv::SettingsDialog& dialog = rig.dialog(false);
    dialog.ensurePage(dest::dictionary);
    QStringList titles;
    for (const fv::appcore::SearchEntry& entry : fv::buildSearchIndex(dialog)) {
        if (entry.destination == dest::dictionary) {
            titles << QString::fromStdString(entry.title);
        }
    }
    CHECK(titles.contains(QStringLiteral("Shown")));
    CHECK(titles.contains(QStringLiteral("Folded away")));
    CHECK_FALSE(titles.contains(QStringLiteral("A choice")));
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_search_finds_the_actions_page_but_not_its_gallery
TEST_CASE("test_search_finds_the_actions_page_but_not_its_gallery",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAskClaude}));
    fv::SettingsDialog& dialog = rig.dialog(false);
    // Not built yet: the page's own search fields.
    CHECK(containsAll(actionsTitles(dialog),
                      {QStringLiteral("Agent hotkey"), QStringLiteral("Your actions"),
                       QStringLiteral("Route by voice")}));
    dialog.selectDestination(qs(dest::actions));
    pump();
    QStringList titles = actionsTitles(dialog);
    CHECK(containsAll(titles, {QStringLiteral("Agent hotkey"), QStringLiteral("Ask Claude"),
                               QStringLiteral("Route by voice")}));
    CHECK_FALSE(containsAny(titles, kPresetNames));
    rig.panel().addAction();
    pump();
    CHECK_FALSE(containsAny(actionsTitles(dialog), kPresetNames));
    // Built, but another page is showing: its tiles are still found.
    rig.panel().showList();
    dialog.selectDestination(qs(dest::general));
    pump();
    titles = actionsTitles(dialog);
    CHECK(containsAll(titles, {QStringLiteral("Agent hotkey"), QStringLiteral("Ask Claude"),
                               QStringLiteral("Route by voice")}));
    CHECK_FALSE(containsAny(titles, kPresetNames));
}

// ---- 12. "What it's for" starts empty ----

// from tests/test_automation_fix_ui_actions_page.py::test_a_preset_leaves_what_its_for_to_you
TEST_CASE("test_a_preset_leaves_what_its_for_to_you",
          "[test_automation_fix_ui_actions_page][widget]") {
    for (const char* preset : {"claude-chat", "quick-answer", "webhook", "copy"}) {
        INFO(preset);
        ActionsRig rig;
        rig.dialog();
        startFrom(rig.panel(), preset);
        fv::ActionEditor& editor = rig.editor();
        CHECK(editor.descriptionEdit()->text().isEmpty());
        CHECK(editor.descriptionEdit()->placeholderText().startsWith(
            QStringLiteral("What it's for")));
        editor.urlEdit()->setText(QStringLiteral("https://example.com/hook"));
        CHECK(editor.save());
        CHECK(rig.stored(fv::qt::toUtf8(editor.actionId()))->description.empty());
    }
}

// ---- 13. The agent you pick before setting up stays picked ----

// from
// tests/test_automation_fix_ui_actions_page.py::test_the_agent_you_pick_before_setup_stays_picked
TEST_CASE("test_the_agent_you_pick_before_setup_stays_picked",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig(withActions({kAskClaude}));
    rig.dialog();
    fv::AgentHotkeyCard& card = rig.card();
    card.fields()->chooser()->choice(QStringLiteral("codex"))->click();
    rig.panel().setActionEnabled(QStringLiteral("a1"), false);
    CHECK(card.fields()->chooser()->current() == QStringLiteral("codex"));
    card.lookAgain()->click();
    CHECK(waitFor([&] { return card.fields()->chooser()->available().has_value(); }));
    CHECK(card.fields()->chooser()->current() == QStringLiteral("codex"));
    card.setupButton()->click();
    card.cancelCapture();
    CHECK(rig.stored("agent")->param("agent") == "codex");
}

// from
// tests/test_automation_fix_ui_actions_page.py::test_a_picked_agent_that_goes_missing_gives_way
TEST_CASE("test_a_picked_agent_that_goes_missing_gives_way",
          "[test_automation_fix_ui_actions_page][widget]") {
    ActionsRig rig;
    rig.dialog();
    fv::AgentHotkeyCard& card = rig.card();
    card.fields()->chooser()->choice(QStringLiteral("codex"))->click();
    rig.setFoundOnNextScan({{"claude_code", true}, {"codex", false}, {"opencode_cli", false}});
    card.lookAgain()->click();
    CHECK(waitFor([&] { return card.fields()->chooser()->available().has_value(); }));
    CHECK(card.fields()->chooser()->current() == QStringLiteral("claude_code"));
}
