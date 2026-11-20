// AgentChoice, AgentChooser and AgentFields (ui_qt/widgets/action_fields.py): only found agents
// can be chosen, a missing chosen agent stays shown, the chooser stacks when its names don't
// fit, and the form keeps parameters it doesn't show, shows a quick answer's "Nowhere" as the
// Scratchpad, moves OpenCode off App, and hides the working folder for new folders.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>

#include <QApplication>
#include <QLineEdit>
#include <QSignalSpy>
#include <QVariantAnimation>

#include "automation_support.h"
#include "base/json.h"
#include "ui_automation/action_fields.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/agent_mark.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::AgentChooser;
using fv::AgentFields;
using fv::Json;

// The scan of the reference builder: Claude Code and Codex found, OpenCode missing.
const std::map<std::string, bool> kScan{
    {"claude_code", true}, {"codex", true}, {"opencode_cli", false}};

QString detailOf(AgentChooser& chooser, const char* agent) {
    return chooser.choice(QString::fromLatin1(agent))->detailLabel()->text();
}

} // namespace

TEST_CASE("agent choices read Checking until a scan says what was found", "[widget]") {
    AgentChooser chooser;
    REQUIRE(chooser.choices().size() == 3);
    CHECK(chooser.choices()[0]->agentId() == QStringLiteral("claude_code"));
    CHECK(chooser.choices()[1]->agentId() == QStringLiteral("codex"));
    CHECK(chooser.choices()[2]->agentId() == QStringLiteral("opencode_cli"));
    CHECK(chooser.choices()[0]->accessibleName() == QStringLiteral("Claude Code"));
    CHECK(chooser.choices()[2]->nameLabel()->text() == QStringLiteral("OpenCode"));
    CHECK(detailOf(chooser, "codex") == QStringLiteral("Checking…"));
    CHECK_FALSE(chooser.available().has_value());
    for (fv::AgentChoice* choice : chooser.choices()) {
        CHECK_FALSE(choice->isEnabled());
    }

    chooser.setAvailable(kScan, true);
    CHECK(detailOf(chooser, "claude_code") == QStringLiteral("Found"));
    CHECK(detailOf(chooser, "opencode_cli") == QStringLiteral("Not found"));
    CHECK(chooser.choice(QStringLiteral("codex"))->detailLabel()->property(fv::ui::prop::tone) ==
          QStringLiteral("found"));
    CHECK(chooser.choice(QStringLiteral("opencode_cli"))
              ->detailLabel()
              ->property(fv::ui::prop::tone) == QStringLiteral("missing"));
    CHECK(chooser.choice(QStringLiteral("opencode_cli"))->accessibleDescription() ==
          QStringLiteral("Not found"));
    CHECK(chooser.choice(QStringLiteral("codex"))->mark()->warmth() == 1.0);
    CHECK(chooser.choice(QStringLiteral("opencode_cli"))->mark()->missing() == 1.0);
    CHECK(chooser.found() ==
          std::vector<QString>{QStringLiteral("claude_code"), QStringLiteral("codex")});

    // Looking again.
    chooser.setAvailable({}, false);
    CHECK(detailOf(chooser, "claude_code") == QStringLiteral("Checking…"));
    CHECK(chooser.found().empty());
}

TEST_CASE("only found agents can be chosen", "[widget]") {
    AgentChooser chooser;
    chooser.setAvailable(kScan, true);
    CHECK(chooser.choice(QStringLiteral("claude_code"))->isEnabled());
    CHECK(chooser.choice(QStringLiteral("codex"))->isEnabled());
    CHECK_FALSE(chooser.choice(QStringLiteral("opencode_cli"))->isEnabled());

    QSignalSpy chosen(&chooser, &AgentChooser::chosen);
    chooser.choice(QStringLiteral("codex"))->click();
    REQUIRE(chosen.count() == 1);
    CHECK(chosen.at(0).at(0).toString() == QStringLiteral("codex"));
    CHECK(chooser.current() == QStringLiteral("codex"));
    CHECK(chooser.choice(QStringLiteral("codex"))->isChecked());
    CHECK_FALSE(chooser.choice(QStringLiteral("claude_code"))->isChecked());
    // The chosen name carries the selected look.
    CHECK(chooser.choice(QStringLiteral("codex"))->nameLabel()->property(fv::ui::prop::selected) ==
          true);

    // Clicking the chosen one again says nothing.
    chooser.choice(QStringLiteral("codex"))->click();
    CHECK(chosen.count() == 1);
    CHECK(chooser.choice(QStringLiteral("codex"))->isChecked());
}

TEST_CASE("a missing chosen agent stays checked and disabled", "[widget]") {
    AgentChooser chooser;
    chooser.setCurrent(QStringLiteral("opencode_cli"));
    chooser.setAvailable(kScan, true);
    fv::AgentChoice* opencode = chooser.choice(QStringLiteral("opencode_cli"));
    CHECK(opencode->isChecked());
    CHECK_FALSE(opencode->isEnabled());
    CHECK(chooser.current() == QStringLiteral("opencode_cli"));

    // An id that is not an agent chooses the first.
    chooser.setCurrent(QStringLiteral("cursor"));
    CHECK(chooser.current() == QStringLiteral("claude_code"));
}

TEST_CASE("a found agent's mark warms up over the arrival", "[widget]") {
    AgentChooser chooser;
    chooser.setAvailable(kScan, true, true);
    fv::AgentChoice* claude = chooser.choice(QStringLiteral("claude_code"));
    CHECK(claude->arrival()->state() == QAbstractAnimation::Running);
    CHECK(claude->arrival()->duration() == 380);
    CHECK(claude->arrival()->easingCurve().type() == QEasingCurve::OutCubic);
    CHECK(claude->mark()->warmth() < 1.0);
    claude->arrival()->setCurrentTime(380);
    CHECK(claude->mark()->warmth() == 1.0);
    // A missing one has nothing to warm.
    CHECK(chooser.choice(QStringLiteral("opencode_cli"))->arrival()->state() ==
          QAbstractAnimation::Stopped);
}

TEST_CASE("the agent chooser stacks below the width of its names", "[widget]") {
    fvtest::AppTheme theme;
    // In a page that gives it any width, as Settings pages do.
    QWidget page;
    page.resize(1000, 300);
    auto* chooser = new AgentChooser(&page);
    chooser->setAvailable(kScan, true);
    chooser->setGeometry(0, 0, 900, 200);
    page.show();
    QApplication::processEvents();
    // Measured once styled: the theme sets the names' font.
    int needed = 0;
    for (fv::AgentChoice* choice : chooser->choices()) {
        needed += choice->sizeHint().width();
    }
    needed += 8 * 2;
    chooser->setGeometry(0, 0, needed, 200);
    QApplication::processEvents();
    CHECK_FALSE(chooser->isStacked());
    chooser->setGeometry(0, 0, needed - 1, 200);
    QApplication::processEvents();
    CHECK(chooser->isStacked());
    CHECK(chooser->choices()[1]->y() > chooser->choices()[0]->y());
    chooser->setGeometry(0, 0, needed + 40, 200);
    QApplication::processEvents();
    CHECK_FALSE(chooser->isStacked());
    for (fv::AgentChoice* choice : chooser->choices()) {
        CHECK(choice->sizeHint().height() >= 44);
    }
}

TEST_CASE("agent fields keep the parameters they don't show", "[widget]") {
    AgentFields fields;
    QSignalSpy changed(&fields, &AgentFields::changed);
    const Json given{{"agent", "codex"},      {"mode", "ask"},    {"later", Json{{"x", 1}}},
                     {"surface", "terminal"}, {"model", " gpt "}, {"output", "notice"},
                     {"flag", true}};
    fields.setParams(given);
    // A programmatic fill is not the person's change.
    CHECK(changed.count() == 0);
    const Json params = fields.params();
    CHECK(params["later"] == Json{{"x", 1}});
    CHECK(params["flag"] == true);
    CHECK(params["agent"] == "codex");
    CHECK(params["mode"] == "ask");
    CHECK(params["surface"] == "terminal");
    CHECK(params["model"] == "gpt");
    CHECK(params["output"] == "notice");
    CHECK(params["folder"] == "");
    CHECK(params["new_folder"] == false);
    CHECK(params["include_selection"] == false);
    // The unknown ones first, then the form's own, in their order.
    std::vector<std::string> keys;
    for (const auto& [key, value] : params.items()) {
        keys.push_back(key);
    }
    CHECK(keys == std::vector<std::string>{"later", "flag", "agent", "mode", "surface", "folder",
                                           "new_folder", "include_selection", "model", "output"});

    // A new fill forgets the old extras.
    fields.setParams(Json{{"agent", "claude_code"}});
    CHECK_FALSE(fields.params().contains("later"));
}

TEST_CASE("agent fields start from the agent kind's defaults", "[widget]") {
    AgentFields fields;
    const Json params = fields.params();
    CHECK(params == Json{{"agent", "claude_code"},
                         {"mode", "handoff"},
                         {"surface", "auto"},
                         {"folder", ""},
                         {"new_folder", false},
                         {"include_selection", false},
                         {"model", ""},
                         {"output", "scratchpad"}});
    // Unknown values fall back.
    fields.setParams(Json{{"agent", 3}, {"mode", "sing"}, {"surface", "moon"}, {"output", 7}});
    CHECK(fields.params()["agent"] == "claude_code");
    CHECK(fields.params()["mode"] == "handoff");
    CHECK(fields.params()["surface"] == "auto");
    CHECK(fields.params()["output"] == "paste");
}

TEST_CASE("a quick answer that goes nowhere shows as the Scratchpad", "[widget]") {
    AgentFields fields;
    fields.setParams(Json{{"mode", "ask"}, {"output", "none"}});
    CHECK(fields.outputCombo()->currentText() == QStringLiteral("Scratchpad"));
    CHECK(fields.params()["output"] == "scratchpad");
    // The answer always goes somewhere: Nowhere is not offered.
    CHECK(fields.outputCombo()->findData(QStringLiteral("none")) < 0);
    CHECK(fields.outputCombo()->count() == 4);
}

TEST_CASE("a session and a quick answer show their own fields", "[widget]") {
    fvtest::AppTheme theme;
    AgentFields fields;
    fields.resize(600, 800);
    fields.show();
    fields.setParams(Json{{"agent", "claude_code"}});
    CHECK(fields.sessionBox()->isVisible());
    CHECK_FALSE(fields.answerBox()->isVisible());
    CHECK(fields.modeHint()->text() ==
          QStringLiteral("Claude Code opens in a new session with your words."));
    CHECK(fields.surfaceHint()->text().isEmpty() == false);

    QSignalSpy changed(&fields, &AgentFields::changed);
    fields.modeBar()->buttons()[1]->click();
    CHECK(changed.count() == 1);
    CHECK_FALSE(fields.sessionBox()->isVisible());
    CHECK(fields.answerBox()->isVisible());
    CHECK(
        fields.modeHint()->text() ==
        QStringLiteral("Claude Code answers with no tools, so nothing on this computer changes."));
    CHECK(fields.params()["mode"] == "ask");
}

TEST_CASE("OpenCode has no app: App switches to Auto and is disabled", "[widget]") {
    AgentFields fields;
    fields.setAvailable({{"claude_code", true}, {"codex", true}, {"opencode_cli", true}}, true);
    fields.setParams(Json{{"agent", "claude_code"}, {"surface", "app"}});
    fv::SegmentButton* app = fields.surfaceBar()->buttons()[1];
    CHECK(app->isEnabled());
    CHECK(fields.params()["surface"] == "app");

    QSignalSpy changed(&fields, &AgentFields::changed);
    fields.chooser()->choice(QStringLiteral("opencode_cli"))->click();
    // One change: the agent; the surface followed silently.
    CHECK(changed.count() == 1);
    CHECK(fields.params()["agent"] == "opencode_cli");
    CHECK(fields.params()["surface"] == "auto");
    CHECK_FALSE(app->isEnabled());
    CHECK(app->toolTip() == QStringLiteral("OpenCode has no app; it opens in a terminal."));
    CHECK(fields.surfaceHint()->text() ==
          QStringLiteral("OpenCode has no app, so it opens in a new terminal and starts on your "
                         "words right away."));

    fields.chooser()->choice(QStringLiteral("codex"))->click();
    CHECK(app->isEnabled());
    CHECK(app->toolTip().isEmpty());
}

TEST_CASE("a new folder per request hides the working folder", "[widget]") {
    fvtest::AppTheme theme;
    AgentFields fields;
    fields.resize(600, 800);
    fields.show();
    fields.setParams(Json{{"folder", "C:\\Projects"}});
    CHECK(fields.folderBox()->isVisible());
    CHECK(fields.folder()->path() == QStringLiteral("C:\\Projects"));
    QSignalSpy changed(&fields, &AgentFields::changed);
    fields.newFolderRow()->switchButton()->toggle();
    CHECK(changed.count() == 1);
    CHECK_FALSE(fields.folderBox()->isVisible());
    // The folder is kept for when it is turned off again.
    CHECK(fields.params()["folder"] == "C:\\Projects");
    CHECK(fields.params()["new_folder"] == true);
    CHECK(fields.newFolderRow()->detailLabel()->text() ==
          QStringLiteral("A fresh folder per request in ~/FioriVoce Agent Tasks."));

    fields.setParams(Json{{"agent", "codex"}, {"new_folder", 1}});
    CHECK_FALSE(fields.folderBox()->isVisible());
    CHECK(fields.newFolderRow()->detailLabel()->text() ==
          QStringLiteral("A fresh folder per request in ~/FioriVoce Agent Tasks. The Codex app "
                         "adds it as a local project."));
    CHECK(fields.newFolderRow()->switchButton()->accessibleDescription() ==
          fields.newFolderRow()->detailLabel()->text());
    fields.setParams(Json{{"agent", "codex"}, {"surface", "terminal"}});
    CHECK(fields.newFolderRow()->detailLabel()->text() ==
          QStringLiteral("A fresh folder per request in ~/FioriVoce Agent Tasks."));
}

TEST_CASE("a missing saved agent is named under the chooser", "[widget]") {
    fvtest::AppTheme theme;
    AgentFields fields;
    fields.resize(600, 800);
    fields.show();
    fields.setParams(Json{{"agent", "opencode_cli"}});
    // Not looked yet: nothing to say.
    CHECK_FALSE(fields.agentNote()->isVisible());
    fields.setAvailable(kScan, true);
    CHECK(fields.agentNote()->isVisible());
    CHECK(fields.agentNote()->text() ==
          QStringLiteral("OpenCode wasn't found on this computer. Choose another agent."));
    fields.setAvailable({}, true);
    CHECK(fields.agentNote()->text() == QStringLiteral("OpenCode wasn't found on this computer."));
    // A card inviting setup says it itself.
    fields.setCompact(true);
    CHECK_FALSE(fields.agentNote()->isVisible());
    CHECK_FALSE(fields.details()->isVisible());
    fields.setCompact(false);
    CHECK(fields.details()->isVisible());
    CHECK(fields.agentNote()->isVisible());
    fields.setAvailable(kScan, true);
    fields.setParams(Json{{"agent", "codex"}});
    CHECK_FALSE(fields.agentNote()->isVisible());
}

TEST_CASE("an included selection points at app awareness when it is off", "[widget]") {
    fvtest::AppTheme theme;
    AgentFields fields;
    fields.resize(600, 800);
    fields.show();
    fields.setParams(Json{{"include_selection", true}});
    CHECK_FALSE(fields.selectionNote()->isVisible());
    fields.setAppAwareness(false);
    CHECK(fields.selectionNote()->isVisible());
    CHECK(fields.selectionHint()->text() ==
          QStringLiteral(u"Needs “Know which app I'm dictating into” on Styles."));
    QSignalSpy styles(&fields, &AgentFields::stylesRequested);
    fields.stylesLink()->click();
    CHECK(styles.count() == 1);
    fields.selectionRow()->switchButton()->toggle();
    CHECK_FALSE(fields.selectionNote()->isVisible());
}

TEST_CASE("an edited model is a change only once it was edited", "[widget]") {
    AgentFields fields;
    fields.setParams(Json{{"mode", "ask"}});
    QSignalSpy changed(&fields, &AgentFields::changed);
    Q_EMIT fields.modelEdit()->editingFinished();
    CHECK(changed.count() == 0);
    fields.modelEdit()->setText(QStringLiteral("haiku"));
    fields.modelEdit()->setModified(true);
    Q_EMIT fields.modelEdit()->editingFinished();
    CHECK(changed.count() == 1);
    CHECK_FALSE(fields.modelEdit()->isModified());
    CHECK(fields.params()["model"] == "haiku");
    CHECK(fields.modelEdit()->maxLength() == 120);
}

TEST_CASE("agent fields cap their widths at the font scale", "[widget]") {
    fvtest::AppTheme theme;
    AgentFields fields;
    CHECK(fields.chooser()->maximumWidth() == 660);
    CHECK(fields.modeBar()->maximumWidth() == 460);
    CHECK(fields.outputCombo()->maximumWidth() == 340);
    theme.setScale(130);
    QApplication::processEvents();
    CHECK(fields.chooser()->maximumWidth() == 858);
    CHECK(fields.modeBar()->maximumWidth() == 598);
    CHECK(fields.outputCombo()->maximumWidth() == 442);
}
