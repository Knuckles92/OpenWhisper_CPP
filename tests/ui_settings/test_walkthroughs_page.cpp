// Settings > Dictation > Walkthroughs (settings_dialog.py _build_walkthroughs_page), from
// tests/test_settings_general_layout.py and the checks that page owes: what it binds, how the
// Agent combo reads the installed-agent scan, and that its shortcut rows are the Hotkeys page's.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <QFocusEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTest>
#include <QTextDocument>

#include "hotkeys/defaults.h"
#include "hotkeys/grammar.h"
#include "hotkeys/testing/fake_capture.h"
#include "settings/enums.h"
#include "settings/keys.h"
#include "ui_hotkeys/hotkey_capture.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_settings/walkthroughs_page.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"
#include "walkthrough/write_up_names.h"
#include "walkthroughs_support.h"

using fv::Json;
using namespace fvtest;

namespace {

QStringList texts(QWidget& page, const char* objectName) {
    QStringList out;
    for (auto* label : page.findChildren<QLabel*>(QString::fromLatin1(objectName))) {
        out.append(label->text());
    }
    return out;
}

void leave(QWidget* edit) {
    QFocusEvent event(QEvent::FocusOut);
    QApplication::sendEvent(edit, &event);
}

// The text a combo shows for each of its items.
QStringList itemTexts(const QComboBox& combo) {
    QStringList out;
    for (int i = 0; i < combo.count(); ++i) {
        out.append(combo.itemText(i));
    }
    return out;
}

QString display(const std::string& hotkey) {
    return QString::fromStdString(
        fv::hotkeys::format_hotkey_display(hotkey, fv::hotkeys::platform_vocabulary()));
}

} // namespace

// from
// tests/test_settings_general_layout.py::TestSettingsGeneralLayout::test_walkthrough_copy_preference_saves_and_reloads
TEST_CASE("walkthrough copy preference saves and reloads", "[test_settings_general_layout]") {
    WalkthroughsRig scene;
    {
        auto& page = scene.page();
        auto* combo = page.copyFormatCombo();
        CHECK(combo->currentData().toString() == QStringLiteral("pdf"));
        QStringList values;
        for (int i = 0; i < combo->count(); ++i) {
            values.append(combo->itemData(i).toString());
        }
        CHECK(values == QStringList{QStringLiteral("pdf"), QStringLiteral("agent_task"),
                                    QStringLiteral("both"), QStringLiteral("agent"),
                                    QStringLiteral("word")});
        for (const char* value : {"agent", "both", "agent_task"}) {
            combo->setCurrentIndex(combo->findData(QString::fromLatin1(value)));
            CHECK(scene.saved("walkthrough_copy_format") == Json(value));
        }
    }
    // A new window reads the saved choice.
    scene.rig().dropDialog();
    CHECK(scene.page().copyFormatCombo()->currentData().toString() == QStringLiteral("agent_task"));
}

TEST_CASE("the page lists its groups and tiles as OpenWhisper's does", "[walkthroughs_page]") {
    WalkthroughsRig scene;
    auto& page = scene.page();
    CHECK(texts(page, "hotkeyGroupTitle") == QStringList{QStringLiteral("Shortcuts")});
    CHECK(texts(page, "settingsTileGroupTitle") == QStringList{QStringLiteral("Copy"),
                                                               QStringLiteral("Document"),
                                                               QStringLiteral("AI write-up")});

    // The field tiles the page holds, and the notes tile among its switches.
    const auto fields = page.findChildren<fv::FieldTile*>();
    CHECK(std::set<fv::FieldTile*>(fields.begin(), fields.end()) ==
          std::set<fv::FieldTile*>{page.titleTile(), page.pageSizeTile(), page.copyFormatTile(),
                                   page.agentTile(), page.instructionsTile()});
    CHECK(page.isAncestorOf(page.notesTile()));

    CHECK(page.copyFormatCombo()->objectName() == QStringLiteral("walkthroughCopyFormatCombo"));
    CHECK(page.copyFormatCombo()->accessibleName() == QStringLiteral("Walkthrough default copy"));
    CHECK(page.copyFormatCombo()->minimumHeight() == 40);
    CHECK(itemTexts(*page.copyFormatCombo()) ==
          QStringList{QStringLiteral("PDF copy"), QStringLiteral("Agent (PDF + instructions)"),
                      QStringLiteral("PDF and Markdown copy"), QStringLiteral("Markdown copy"),
                      QStringLiteral("Word document copy")});
    CHECK(page.titleEdit()->placeholderText() == QStringLiteral("No title"));
    CHECK(page.titleEdit()->accessibleName() == QStringLiteral("Walkthrough document title"));
    CHECK(itemTexts(*page.pageSizeCombo()) ==
          QStringList{QStringLiteral("Automatic (Letter or A4 for your region)"),
                      QStringLiteral("Letter"), QStringLiteral("A4")});
    CHECK(page.instructionsEdit()->accessibleName() == QStringLiteral("Write-up instructions"));
    CHECK(page.instructionsEdit()->minimumHeight() == 96);
    CHECK(page.instructionsEdit()->maximumHeight() == 150);
    CHECK(page.instructionsEdit()->placeholderText().toStdString() ==
          std::string(fv::walkthrough::write_up::DEFAULT_INSTRUCTIONS));
    CHECK(page.writeUpTile()->titleLabel()->text() ==
          QStringLiteral("Write it up with an AI agent"));
    CHECK(page.notesTile()->titleLabel()->text() ==
          QStringLiteral("Follow notes left for the agent"));
}

TEST_CASE("every setting of the page loads its saved value", "[walkthroughs_page]") {
    WalkthroughsRig scene(Json{{"walkthrough_copy_format", "word"},
                               {"walkthrough_title", "Bug report"},
                               {"walkthrough_page_size", "a4"},
                               {"walkthrough_show_details", false},
                               {"walkthrough_show_about", false},
                               {"walkthrough_show_captions", false},
                               {"walkthrough_write_up", true},
                               {"walkthrough_write_up_agent", "opencode_cli"},
                               {"walkthrough_write_up_instructions", "Make it a checklist."},
                               {"walkthrough_write_up_notes", true}});
    auto& page = scene.page();
    CHECK(page.copyFormatCombo()->currentData().toString() == QStringLiteral("word"));
    CHECK(page.titleEdit()->text() == QStringLiteral("Bug report"));
    CHECK(page.pageSizeCombo()->currentData().toString() == QStringLiteral("a4"));
    CHECK_FALSE(page.detailsTile()->checkbox()->isChecked());
    CHECK_FALSE(page.aboutTile()->checkbox()->isChecked());
    CHECK_FALSE(page.captionsTile()->checkbox()->isChecked());
    CHECK(page.writeUpTile()->checkbox()->isChecked());
    CHECK(page.agentCombo()->currentData().toString() == QStringLiteral("opencode_cli"));
    CHECK(page.instructionsEdit()->toPlainText() == QStringLiteral("Make it a checklist."));
    CHECK(page.notesTile()->checkbox()->isChecked());
    // Loading saved nothing back.
    CHECK(scene.saved("walkthrough_copy_format") == Json("word"));
}

TEST_CASE("the defaults are what a fresh install shows", "[walkthroughs_page]") {
    WalkthroughsRig scene;
    auto& page = scene.page();
    CHECK(page.copyFormatCombo()->currentData().toString() == QStringLiteral("pdf"));
    CHECK(page.titleEdit()->text() == QStringLiteral("Walkthrough"));
    CHECK(page.pageSizeCombo()->currentData().toString() == QStringLiteral("auto"));
    CHECK(page.detailsTile()->checkbox()->isChecked());
    CHECK(page.aboutTile()->checkbox()->isChecked());
    CHECK(page.captionsTile()->checkbox()->isChecked());
    CHECK_FALSE(page.writeUpTile()->checkbox()->isChecked());
    CHECK(page.agentCombo()->currentData().toString() == QStringLiteral("claude_code"));
    CHECK(page.instructionsEdit()->toPlainText().isEmpty());
    CHECK_FALSE(page.notesTile()->checkbox()->isChecked());
    // Nothing was saved by loading the defaults.
    CHECK_FALSE(scene.has("walkthrough_copy_format"));
    CHECK_FALSE(scene.has("walkthrough_title"));
}

TEST_CASE("each control saves its key when it changes", "[walkthroughs_page]") {
    WalkthroughsRig scene;
    auto& page = scene.page();

    page.titleEdit()->setText(QStringLiteral("  Bug report  "));
    CHECK_FALSE(scene.has("walkthrough_title")); // not while typing
    Q_EMIT page.titleEdit()->editingFinished();
    CHECK(scene.saved("walkthrough_title") == Json("Bug report"));

    page.pageSizeCombo()->setCurrentIndex(page.pageSizeCombo()->findData(QStringLiteral("letter")));
    CHECK(scene.saved("walkthrough_page_size") == Json("letter"));

    page.detailsTile()->checkbox()->setChecked(false);
    CHECK(scene.saved("walkthrough_show_details") == Json(false));
    page.aboutTile()->checkbox()->setChecked(false);
    CHECK(scene.saved("walkthrough_show_about") == Json(false));
    page.captionsTile()->checkbox()->setChecked(false);
    CHECK(scene.saved("walkthrough_show_captions") == Json(false));

    page.writeUpTile()->checkbox()->setChecked(true);
    CHECK(scene.saved("walkthrough_write_up") == Json(true));
    page.agentCombo()->setCurrentIndex(page.agentCombo()->findData(QStringLiteral("codex")));
    CHECK(scene.saved("walkthrough_write_up_agent") == Json("codex"));
    page.notesTile()->checkbox()->setChecked(true);
    CHECK(scene.saved("walkthrough_write_up_notes") == Json(true));
}

TEST_CASE("Follow notes is disabled until the write-up is on", "[walkthroughs_page]") {
    WalkthroughsRig scene;
    auto& page = scene.page();
    CHECK_FALSE(page.notesTile()->isEnabled());
    page.writeUpTile()->checkbox()->setChecked(true);
    CHECK(page.notesTile()->isEnabled());
    page.writeUpTile()->checkbox()->setChecked(false);
    CHECK_FALSE(page.notesTile()->isEnabled());

    // A saved write-up opens with the notes switch already usable.
    WalkthroughsRig saved(Json{{"walkthrough_write_up", true}});
    CHECK(saved.page().notesTile()->isEnabled());
}

TEST_CASE("the instructions editor saves \"\" for the default words", "[walkthroughs_page]") {
    WalkthroughsRig scene;
    auto& page = scene.page();
    QPlainTextEdit* edit = page.instructionsEdit();
    const auto type = [edit](const QString& text) {
        edit->setPlainText(text);
        edit->document()->setModified(true);
    };

    type(QStringLiteral("  Make it a checklist.  "));
    CHECK_FALSE(scene.has("walkthrough_write_up_instructions")); // not while typing
    leave(edit);
    CHECK(scene.saved("walkthrough_write_up_instructions") == Json("Make it a checklist."));

    // The default words, typed or pasted back, are the default: nothing of them is kept.
    type(edit->placeholderText());
    leave(edit);
    CHECK(scene.saved("walkthrough_write_up_instructions") == Json(""));
}

TEST_CASE("the Agent combo names the agents the scan did not find", "[walkthroughs_page]") {
    WalkthroughsRig scene(Json::object(), withoutCodex());
    auto& page = scene.page();
    CHECK(itemTexts(*page.agentCombo()) == QStringList{QStringLiteral("Claude Code"),
                                                       QStringLiteral("Codex (not installed)"),
                                                       QStringLiteral("OpenCode")});
    // The choice behind a label is the agent's id, and a missing agent can still be chosen.
    CHECK(page.agentCombo()->itemData(1).toString() == QStringLiteral("codex"));
    page.agentCombo()->setCurrentIndex(1);
    CHECK(scene.saved("walkthrough_write_up_agent") == Json("codex"));
    CHECK(page.agentCombo()->currentText() == QStringLiteral("Codex (not installed)"));
}

TEST_CASE("with every agent installed no name is marked", "[walkthroughs_page]") {
    WalkthroughsRig scene;
    auto& page = scene.page();
    CHECK(itemTexts(*page.agentCombo()) == QStringList{QStringLiteral("Claude Code"),
                                                       QStringLiteral("Codex"),
                                                       QStringLiteral("OpenCode")});
}

TEST_CASE("the page starts one scan when none is cached, and none when one is",
          "[walkthroughs_page]") {
    WalkthroughsRig scene(Json::object(), withoutCodex());
    CHECK_FALSE(scene.agents().cached_scan().has_value());
    // Before the scan's answer reaches the GUI thread the names are plain: the relabel is posted,
    // and nothing here has run the event loop yet.
    auto& page = scene.rig().page<fv::WalkthroughsPage>(dest::walkthroughs);
    CHECK(itemTexts(*page.agentCombo()) == QStringList{QStringLiteral("Claude Code"),
                                                       QStringLiteral("Codex"),
                                                       QStringLiteral("OpenCode")});
    scene.settle();
    // One scan asked each agent once.
    CHECK(scene.agents().lookups() == 3);
    CHECK(page.agentCombo()->itemText(1) == QStringLiteral("Codex (not installed)"));

    // A page built again reads the cached scan and starts none.
    scene.rig().dropDialog();
    auto& again = scene.page();
    CHECK(scene.agents().lookups() == 3);
    CHECK(again.agentCombo()->itemText(1) == QStringLiteral("Codex (not installed)"));
}

TEST_CASE("a page without installed agents leaves every name plain", "[walkthroughs_page]") {
    WalkthroughsRig scene(Json::object(), everyAgent(), false);
    auto& page = scene.page();
    CHECK(itemTexts(*page.agentCombo()) == QStringList{QStringLiteral("Claude Code"),
                                                       QStringLiteral("Codex"),
                                                       QStringLiteral("OpenCode")});
    CHECK(scene.agents().lookups() == 0);
}

TEST_CASE("the rail says whether the write-up is on", "[walkthroughs_page]") {
    CHECK(fv::WalkthroughsPage::railValue(Json::object()) == QStringLiteral("As spoken"));
    CHECK(fv::WalkthroughsPage::railValue(Json{{"walkthrough_write_up", true}}) ==
          QStringLiteral("AI write-up on"));
    WalkthroughsRig scene;
    scene.dialog().refreshRailValues();
    CHECK(scene.dialog().rail()->value(qs(dest::walkthroughs)) == QStringLiteral("As spoken"));
}

TEST_CASE("the page is registered with the names its controls go by", "[walkthroughs_page]") {
    WalkthroughsRig scene;
    CHECK(scene.dialog().pageTitle(dest::walkthroughs) == QStringLiteral("Walkthroughs"));
    CHECK(
        scene.dialog().pageSubtitle(dest::walkthroughs) ==
        QStringLiteral("Talk while you take screenshots, then paste one document into your agent: "
                       "its shortcuts, how the document looks, and the optional AI write-up."));
    auto& page = scene.page();
    CHECK(scene.dialog().control("walkthrough_copy_format_combo") == page.copyFormatCombo());
    CHECK(scene.dialog().control("walkthrough_write_up_agent_combo") == page.agentCombo());
    CHECK(scene.dialog().control("walkthrough_write_up_notes_check") ==
          page.notesTile()->checkbox());
    CHECK(scene.dialog().control("walkthrough_write_up_edit") == page.instructionsEdit());
    CHECK(scene.dialog().control("walkthrough_title_tile") == page.titleTile());
    CHECK(scene.dialog().control("walkthrough_no_such_control") == nullptr);
}

TEST_CASE("the page's three shortcut rows are the Hotkeys page's", "[walkthroughs_page]") {
    WalkthroughsRig scene;
    auto& page = scene.page();
    auto* hotkeys = dynamic_cast<fv::HotkeysPage*>(scene.dialog().pageContent(dest::hotkeys));
    REQUIRE(hotkeys != nullptr);
    for (const char* key : {"paste_walkthrough", "walkthrough", "screenshot"}) {
        INFO(key);
        QWidget* mine = page.shortcutInput(key);
        REQUIRE(mine != nullptr);
        CHECK(page.isAncestorOf(mine));
        QWidget* theirs = hotkeys->hotkeyInput(key);
        REQUIRE(theirs != nullptr);
        CHECK(hotkeys->isAncestorOf(theirs));
        CHECK(mine != theirs);
        // The same standard shortcut, shown on both.
        CHECK(static_cast<fv::HotkeyCaptureInput*>(mine)->text() ==
              static_cast<fv::HotkeyCaptureInput*>(theirs)->text());
    }
    CHECK(page.shortcutInput("record_toggle") == nullptr);
}

TEST_CASE("the shortcut rows read the saved shortcuts", "[walkthroughs_page]") {
    WalkthroughsRig scene(Json{{"hotkeys", {{"walkthrough", "ctrl+alt+w"}}}});
    auto& page = scene.page();
    CHECK(static_cast<fv::HotkeyCaptureInput*>(page.shortcutInput("walkthrough"))->text() ==
          display("ctrl+alt+w"));
}

TEST_CASE("leaving the page stops a shortcut capture it started", "[walkthroughs_page]") {
    WalkthroughsRig scene;
    auto& page = scene.page();
    auto* hotkeys = dynamic_cast<fv::HotkeysPage*>(scene.dialog().pageContent(dest::hotkeys));
    REQUIRE(hotkeys != nullptr);
    fv::hotkeys::FakeCaptureFactory captures;
    hotkeys->setCaptureFactory(captures.factory());
    scene.dialog().show();
    QTest::mouseClick(page.shortcutInput("walkthrough"), Qt::LeftButton);
    CHECK(hotkeys->capturingKey() == std::optional<std::string>("walkthrough"));
    page.cancelCapture();
    CHECK_FALSE(hotkeys->capturingKey().has_value());
    scene.dialog().hide();
}
