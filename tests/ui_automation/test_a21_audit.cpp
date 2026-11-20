// Checks the M10 sweep (auditor AU7) added for rows of docs/port/map/a21-actions-automation-ui.md
// whose code was right but whose exact words, names and limits no ported test pinned.

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>

#include "actions_page_support.h"
#include "automation/action_copy.h"
#include "ui_automation/action_list.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using fv::automation::Action;
using fv::automation::ActionResult;
using namespace fvtest;

namespace {

const Json kAgent{{"id", "agent"},
                  {"name", "Agent hotkey"},
                  {"kind", "agent"},
                  {"params", {{"agent", "claude_code"}}},
                  {"hotkey", "ctrl+alt+shift+space"},
                  {"spoken_names", Json::array({"Claude Code"})},
                  {"description", "Coding tasks for Claude Code"}};

Json withActions(std::vector<Json> actions) {
    Json values = Json::object();
    values["actions"] = Json(std::move(actions));
    return values;
}

Json clipboardAction(int n) {
    return Json{{"id", "c" + std::to_string(n)},
                {"name", "Copy " + std::to_string(n)},
                {"kind", "clipboard"},
                {"params", Json::object()}};
}

} // namespace

// A21-03, A21-68: the invite state's words and the names the card's controls carry.
TEST_CASE("a21 audit: the agent hotkey card invites with OpenWhisper's words", "[widget]") {
    ActionsRig rig;
    rig.dialog();
    fv::AgentHotkeyCard& card = rig.card();
    CHECK(card.titleLabel()->text() == QStringLiteral("Agent hotkey"));
    CHECK(card.descriptionLabel()->text() ==
          QStringLiteral("Press a shortcut, say what you need, and your coding agent takes it "
                         "from there."));
    CHECK(fv::action_agent_card::LOOKING == "Looking for Claude Code, Codex or OpenCode…");
    CHECK(fv::action_agent_card::NONE_FOUND == "None found — install one, or use another action.");
    CHECK(card.setupButton()->text() == QStringLiteral("Set it up"));
    CHECK(card.setupButton()->accessibleName() == QStringLiteral("Set up the agent hotkey"));
    CHECK(card.lookAgain()->text() == QStringLiteral("Look again"));
    CHECK(card.lookAgain()->toolTip() ==
          QStringLiteral("Check this computer again, for example after installing an agent"));
    CHECK(card.enabledSwitch()->accessibleName() == QStringLiteral("Agent hotkey on"));
    CHECK(card.shortcut()->input()->accessibleName() == QStringLiteral("Agent hotkey shortcut"));
    CHECK(card.tryButton()->text() == QStringLiteral("Try it"));
    CHECK(card.tryButton()->toolTip() ==
          QStringLiteral(u"Sends “Say hello in one short sentence.” to the agent, as if you'd "
                         u"said it"));
    CHECK(card.moreButton()->text() == QStringLiteral("Customize"));
    CHECK(card.moreButton()->accessibleName() ==
          QStringLiteral("Customize the agent hotkey in the full editor"));
    CHECK(card.moreButton()->toolTip() ==
          QStringLiteral("Spoken names, how your words are prepared, and the rest"));
    CHECK(card.removeButton()->text() == QStringLiteral("Remove"));
    CHECK(card.removeButton()->toolTip() ==
          QStringLiteral("Remove the agent hotkey and its shortcut"));
}

// A21-04: set-up says what to do next, and a spoken name another action uses is left off.
TEST_CASE("a21 audit: setting up the agent hotkey retries without a spoken name in use",
          "[widget]") {
    ActionsRig rig(withActions({Json{{"id", "x1"},
                                     {"name", "Other"},
                                     {"kind", "clipboard"},
                                     {"params", Json::object()},
                                     {"spoken_names", Json::array({"Claude Code"})}}}));
    rig.dialog();
    fv::AgentHotkeyCard& card = rig.card();
    card.setupButton()->click();
    const auto saved = rig.stored("agent");
    REQUIRE(saved.has_value());
    CHECK(saved->name == "Agent hotkey");
    CHECK(saved->spoken_names.empty());
    CHECK(saved->description == "Coding tasks for Claude Code");
    CHECK(saved->param("surface") == "auto");
    CHECK(card.message()->text() == QStringLiteral("Now press the shortcut you'd like to use."));
    card.shortcut()->input()->cancelCapture();
}

// A21-05, A21-07: the configured card's description and the shortcut messages.
TEST_CASE("a21 audit: the agent hotkey's shortcut messages", "[widget]") {
    ActionsRig rig(withActions({kAgent}));
    rig.dialog();
    fv::AgentHotkeyCard& card = rig.card();
    CHECK(card.descriptionLabel()->text() ==
          qs(fv::automation::action_copy::summary(*rig.stored("agent"))) + QStringLiteral("."));
    Q_EMIT card.shortcut()->captured(QStringLiteral("ctrl+alt+a"));
    CHECK(card.message()->text() == QStringLiteral("Press %1, say what you need, and press it "
                                                   "again.")
                                        .arg(display("ctrl+alt+a")));
    CHECK(card.message()->property("tone").toString() == QStringLiteral("ok"));
    Q_EMIT card.shortcut()->captured(QString());
    CHECK(rig.stored("agent")->hotkey.empty());
    CHECK(card.message()->text() ==
          QStringLiteral("Shortcut removed. Route by voice and the API can still use it."));
}

// A21-08: Try it sends the sample sentence and shows the answer.
TEST_CASE("a21 audit: the agent hotkey's Try it sends the sample sentence", "[widget]") {
    ActionsRig rig(withActions({kAgent}));
    std::vector<std::string> samples;
    rig.tester().handler = [&samples](const Action&, const std::string& sample,
                                      std::function<void(ActionResult)> done) {
        samples.push_back(sample);
        done({false, "Claude Code isn't signed in.", "", false});
    };
    rig.dialog();
    fv::AgentHotkeyCard& card = rig.card();
    card.tryButton()->click();
    CHECK(samples == std::vector<std::string>{"Say hello in one short sentence."});
    CHECK(waitFor([&] { return card.tryButton()->isEnabled(); }));
    CHECK(card.message()->text() == QStringLiteral("Claude Code isn't signed in."));
    CHECK(card.message()->property("tone").toString() == QStringLiteral("error"));
}

// A21-09: Remove asks first, and Cancel keeps the hotkey.
TEST_CASE("a21 audit: removing the agent hotkey asks first", "[widget]") {
    ActionsRig rig(withActions({kAgent}));
    rig.dialog();
    fv::AgentHotkeyCard& card = rig.card();
    rig.answer = QMessageBox::Cancel;
    card.removeButton()->click();
    REQUIRE(rig.questions.size() == 1);
    CHECK(rig.questions.back().first == QStringLiteral("Remove the agent hotkey?"));
    CHECK(rig.questions.back().second ==
          QStringLiteral("Remove the agent hotkey and its shortcut?"));
    CHECK(rig.stored("agent").has_value());
    rig.answer = QMessageBox::Yes;
    card.removeButton()->click();
    CHECK_FALSE(rig.stored("agent").has_value());
    CHECK(card.setupRow()->isVisible());
    CHECK(card.fields()->chooser()->current() == QStringLiteral("claude_code"));
}

// A21-14: at 50 actions (the agent hotkey counts) Add action is disabled and says why.
TEST_CASE("a21 audit: Add action stops at 50 actions", "[widget]") {
    std::vector<Json> actions{kAgent};
    for (int n = 1; n < 50; ++n) {
        actions.push_back(clipboardAction(n));
    }
    ActionsRig rig(withActions(actions));
    rig.dialog();
    fv::ActionsPanel& panel = rig.panel();
    CHECK(panel.addButton()->text() == QStringLiteral("Add action"));
    CHECK(panel.addButton()->isVisible());
    CHECK_FALSE(panel.addButton()->isEnabled());
    CHECK(panel.addButton()->toolTip() ==
          QStringLiteral("You can keep up to 50 actions. Delete one to add another."));
    rig.answer = QMessageBox::Yes;
    panel.list()->card(QStringLiteral("c1"))->deleteLink()->click();
    CHECK(panel.addButton()->isEnabled());
    CHECK(panel.addButton()->toolTip().isEmpty());
}

// A21-32: the delete question's words.
TEST_CASE("a21 audit: deleting an action asks with its name", "[widget]") {
    ActionsRig rig(withActions({clipboardAction(1)}));
    rig.dialog();
    rig.answer = QMessageBox::Cancel;
    rig.panel().list()->card(QStringLiteral("c1"))->deleteLink()->click();
    REQUIRE(rig.questions.size() == 1);
    CHECK(rig.questions.back().first == QStringLiteral("Delete action?"));
    CHECK(rig.questions.back().second == QStringLiteral("Delete Copy 1 and its shortcut?"));
    CHECK(rig.actions().size() == 1);
}

namespace {

void startFromPreset(fvtest::ActionsRig& rig, std::string_view id) {
    rig.panel().addAction();
    rig.panel().startFromPreset(rig.preset(id));
}

} // namespace

// A21-19, A21-21, A21-23, A21-70: the editor's own words, limits and names.
TEST_CASE("a21 audit: the action editor's fields carry OpenWhisper's words and limits",
          "[widget]") {
    ActionsRig rig;
    rig.dialog();
    startFromPreset(rig, "webhook");
    fv::ActionEditor& editor = rig.editor();
    CHECK(editor.titleLabel()->text() == QStringLiteral("New action"));
    CHECK(editor.nameEdit()->maxLength() == 80);
    CHECK(editor.nameEdit()->placeholderText() == QStringLiteral("e.g. Ask Claude"));
    CHECK(editor.spokenEdit()->placeholderText() == QStringLiteral("e.g. Claude, ask Claude"));
    CHECK(editor.spokenEdit()->accessibleName() ==
          QStringLiteral("Spoken names, separated by commas"));
    CHECK(editor.descriptionEdit()->maxLength() == 300);
    CHECK(editor.descriptionEdit()->placeholderText() ==
          QStringLiteral("What it's for, e.g. Questions for Claude"));
    CHECK(editor.sampleEdit()->text() == QStringLiteral("What's on my calendar tomorrow?"));
    CHECK(editor.sampleEdit()->accessibleName() == QStringLiteral("Words to try it with"));
    CHECK(editor.saveButton()->text() == QStringLiteral("Save action"));
    CHECK(editor.templateEdit()->maxLength() == 2000);
    CHECK(editor.templateEdit()->placeholderText() ==
          QStringLiteral("https://www.perplexity.ai/search?q={text}"));
    // The webhook kind.
    CHECK(editor.urlEdit()->placeholderText() ==
          QStringLiteral("https://example.com/hooks/%1").arg(qs(fv::identity().product).toLower()));
    CHECK(editor.copySecretButton()->accessibleName() == QStringLiteral("Copy signing secret"));
    CHECK(editor.newSecretButton()->accessibleName() ==
          QStringLiteral("Make a new signing secret"));
    CHECK(editor.secretHint()->text() ==
          QStringLiteral("Your service checks each request with it."));
    CHECK(editor.secretHint()->toolTip() ==
          QStringLiteral("Requests are signed the Standard Webhooks way: webhook-id, "
                         "webhook-timestamp and webhook-signature headers."));
    // The program kind's fields.
    CHECK(editor.argsEdit()->accessibleName() == QStringLiteral("Arguments, one per line"));
    CHECK(editor.argsEdit()->placeholderText() == QStringLiteral("--flag\nvalue"));
    CHECK(editor.argsEdit()->height() == 68);
    CHECK(editor.argsEdit()->tabChangesFocus());
    CHECK(editor.timeoutSpin()->minimum() == 1);
    CHECK(editor.timeoutSpin()->maximum() == 300);
    CHECK(editor.timeoutSpin()->suffix() == QStringLiteral(" s"));
    CHECK(editor.timeoutSpin()->maximumWidth() == 160);
    CHECK(editor.timeoutSpin()->minimumHeight() == 36);
    CHECK(editor.timeoutSpin()->accessibleName() == QStringLiteral("Time limit in seconds"));
    CHECK(editor.mcpLink()->text() == QStringLiteral("Set up the channel"));
    CHECK(editor.mcpLink()->toolTip() == QStringLiteral(u"Settings → Automate → API & MCP"));
}

// A21-24: the note each kind without fields of its own shows.
TEST_CASE("a21 audit: channel, clipboard and scripts actions say what happens", "[widget]") {
    const QString product = qs(fv::identity().product);
    ActionsRig rig;
    rig.dialog();
    startFromPreset(rig, "copy");
    CHECK(rig.editor().kindNote()->text() ==
          QStringLiteral("Your words are copied, ready to paste anywhere. Nothing is pasted for "
                         "you."));
    CHECK_FALSE(rig.editor().mcpLink()->isVisibleTo(&rig.editor()));
    rig.answer = QMessageBox::Discard;
    startFromPreset(rig, "claude-code-session");
    CHECK(rig.editor().kindNote()->text() ==
          QStringLiteral("Needs a Claude Code session running the %1 channel. With none "
                         "listening, your words are copied instead.")
              .arg(product));
    CHECK(rig.editor().mcpLink()->isVisibleTo(&rig.editor()));
    startFromPreset(rig, "scripts");
    CHECK(rig.editor().kindNote()->text() ==
          QStringLiteral("Nothing happens here: programs following %1's live events see your "
                         "words and act on them.")
              .arg(product));
}

// A21-27: Try it runs a probe named "Untitled action" with no way to start it, and a runtime
// that throws says so.
TEST_CASE("a21 audit: Try it sends a nameless probe and reports a runtime failure", "[widget]") {
    ActionsRig rig;
    std::vector<Action> probes;
    rig.tester().handler = [&probes](const Action& action, const std::string&,
                                     std::function<void(ActionResult)>) {
        probes.push_back(action);
        throw std::runtime_error("the runtime is closed");
    };
    rig.dialog();
    startFromPreset(rig, "copy");
    fv::ActionEditor& editor = rig.editor();
    editor.nameEdit()->clear();
    editor.spokenEdit()->setText(QStringLiteral("Stash"));
    Q_EMIT editor.shortcut()->captured(QStringLiteral("ctrl+alt+j"));
    editor.tryIt();
    REQUIRE(probes.size() == 1);
    CHECK(probes[0].name == "Untitled action");
    CHECK(probes[0].hotkey.empty());
    CHECK(probes[0].spoken_names.empty());
    CHECK(waitFor([&] {
        return editor.tryResult()->text() ==
               QStringLiteral("Couldn't try it: the runtime is closed");
    }));
    CHECK(editor.tryResult()->property("tone").toString() == QStringLiteral("error"));
}

// A21-65: the rail icons of Actions and Hooks, and the icons on the Actions page's tiles.
TEST_CASE("a21 audit: Actions and Hooks keep their rail icons", "[unit]") {
    namespace dest = fv::destinations;
    REQUIRE(dest::automate_pages.size() == 3);
    CHECK(dest::automate_pages[0].key == dest::actions);
    CHECK(std::string_view(dest::automate_pages[0].icon) == "bolt-green.svg");
    CHECK(dest::automate_pages[1].key == dest::hooks);
    CHECK(std::string_view(dest::automate_pages[1].icon) == "webhook-blue.svg");
}

// A21-66: what Settings search says about each part of the Actions page.
TEST_CASE("a21 audit: the Actions page's search descriptions", "[unit]") {
    const auto& fields = fv::actions_page::search_fields();
    REQUIRE(fields.size() == 5);
    CHECK(fields[0].description ==
          "Press a shortcut, say what you need, and your coding agent takes it from there.");
    CHECK(fields[1].target == "actions_panel");
    CHECK(fields[1].description == "Send your words to a chat, a webhook or your own program. "
                                   "Give each a shortcut, or say its name.");
    CHECK(fields[2].description ==
          "Say an action's name first, like “Claude, …”, and your words go to it.");
    CHECK(fields[3].description ==
          "A normal dictation that starts with an action's name goes to that action too.");
    CHECK(fields[4].description ==
          "Record, then send your words to the action they name, or paste them as usual.");
    CHECK(fv::actions_page::control_names() ==
          std::vector<std::string>{"actions_agent_card", "actions_panel", "actions_routing_tile",
                                   "actions_routing_dictation_tile", "actions_route_shortcut_tile",
                                   "actions_routing_status", "actions_routing_privacy",
                                   "actions_route_shortcut"});
}
