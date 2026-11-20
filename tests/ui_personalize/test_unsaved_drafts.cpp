// Snippet and transform edits save on leaving, as "Changes save automatically" says
// (tests/test_personalize_fix_f4_unsaved_drafts.py), over the Snippets and Commands pages in a
// real, shown Settings window. Every question is recorded as "title: text" and answered by the
// rig (Cancel unless a test says otherwise).
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTest>
#include <QTextEdit>

#include "styles_commands_support.h"
#include "text/snippets.h"
#include "ui_personalize/snippets_panel.h"
#include "ui_personalize/transforms_panel.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using namespace fvtest;

namespace {

fv::SettingsDialog& shown(PersonalizeRig& rig) {
    fv::SettingsDialog& dialog = rig.window();
    dialog.show();
    return dialog;
}

fv::SnippetsPanel& snippetDraft(PersonalizeRig& rig,
                                const QString& trigger = QStringLiteral("my address"),
                                const QString& text = QStringLiteral("1 Main St")) {
    rig.window().selectDestination(qs(dest::snippets));
    fv::SnippetsPanel& panel = rig.snippetEditor();
    panel.newSnippet();
    panel.triggerEdit()->setText(trigger);
    panel.textEdit()->setPlainText(text);
    CHECK(panel.hasUnsavedChanges());
    return panel;
}

fv::TransformsPanel&
transformDraft(PersonalizeRig& rig, const QString& name = QStringLiteral("Make it a haiku"),
               const QString& instruction = QStringLiteral("Rewrite it as a haiku.")) {
    rig.window().selectDestination(qs(dest::commands));
    fv::TransformsPanel& panel = rig.transforms();
    panel.newTransform();
    panel.nameEdit()->setText(name);
    panel.instructionEdit()->setPlainText(instruction);
    CHECK(panel.hasUnsavedChanges());
    return panel;
}

std::vector<std::string> snippetTriggers(PersonalizeRig& rig) {
    std::vector<std::string> triggers;
    for (const auto& snippet : rig.savedSnippets()) {
        triggers.push_back(snippet.trigger);
    }
    return triggers;
}

bool hasTransform(PersonalizeRig& rig, const std::string& name) {
    for (const auto& transform : rig.savedTransforms()) {
        if (transform.name == name) {
            return true;
        }
    }
    return false;
}

} // namespace

// from tests/test_personalize_fix_f4_unsaved_drafts.py::test_leaving_the_page_saves_a_snippet_draft
TEST_CASE("test_leaving_the_page_saves_a_snippet_draft",
          "[test_personalize_fix_f4_unsaved_drafts][widget]") {
    PersonalizeRig rig;
    fv::SettingsDialog& dialog = shown(rig);
    fv::SnippetsPanel& panel = snippetDraft(rig);

    dialog.selectDestination(qs(dest::general));

    CHECK(snippetTriggers(rig) == std::vector<std::string>{"my address"});
    CHECK_FALSE(panel.hasUnsavedChanges());
    CHECK(rig.asked.isEmpty());
    REQUIRE_FALSE(rig.changes.isEmpty());
    CHECK(rig.changes.back() == QStringLiteral("snippets"));
    dialog.hide();
}

// from tests/test_personalize_fix_f4_unsaved_drafts.py::test_done_saves_a_transform_draft
TEST_CASE("test_done_saves_a_transform_draft", "[test_personalize_fix_f4_unsaved_drafts][widget]") {
    PersonalizeRig rig;
    fv::SettingsDialog& dialog = shown(rig);
    transformDraft(rig);

    dialog.close();

    CHECK(hasTransform(rig, "Make it a haiku"));
    CHECK_FALSE(dialog.isVisible());
    CHECK(rig.asked.isEmpty());
}

// from tests/test_personalize_fix_f4_unsaved_drafts.py::test_escape_saves_drafts_on_every_page
TEST_CASE("test_escape_saves_drafts_on_every_page",
          "[test_personalize_fix_f4_unsaved_drafts][widget]") {
    PersonalizeRig rig;
    fv::SettingsDialog& dialog = shown(rig);
    snippetDraft(rig);
    transformDraft(rig);

    QTest::keyClick(&dialog, Qt::Key_Escape);

    CHECK(snippetTriggers(rig) == std::vector<std::string>{"my address"});
    CHECK(hasTransform(rig, "Make it a haiku"));
    CHECK_FALSE(dialog.isVisible());
}

// from tests/test_personalize_fix_f4_unsaved_drafts.py::test_going_home_saves_a_draft
TEST_CASE("test_going_home_saves_a_draft", "[test_personalize_fix_f4_unsaved_drafts][widget]") {
    PersonalizeRig rig;
    fv::SettingsDialog& dialog = shown(rig);
    snippetDraft(rig);

    dialog.showHome();

    CHECK(snippetTriggers(rig) == std::vector<std::string>{"my address"});
    dialog.hide();
}

// from
// tests/test_personalize_fix_f4_unsaved_drafts.py::test_an_unfinished_draft_stays_when_the_page_is_left
TEST_CASE("test_an_unfinished_draft_stays_when_the_page_is_left",
          "[test_personalize_fix_f4_unsaved_drafts][widget]") {
    PersonalizeRig rig;
    fv::SettingsDialog& dialog = shown(rig);
    fv::SnippetsPanel& panel = snippetDraft(rig, QStringLiteral("my address"), QString());

    dialog.selectDestination(qs(dest::general));
    dialog.selectDestination(qs(dest::snippets));

    CHECK(rig.asked.isEmpty());
    CHECK(snippetTriggers(rig).empty());
    CHECK(panel.triggerEdit()->text() == QStringLiteral("my address"));
    CHECK(panel.message()->text() == QStringLiteral("Add the text to insert."));
    dialog.hide();
}

// from
// tests/test_personalize_fix_f4_unsaved_drafts.py::test_done_with_an_unfinished_draft_can_keep_editing
TEST_CASE("test_done_with_an_unfinished_draft_can_keep_editing",
          "[test_personalize_fix_f4_unsaved_drafts][widget]") {
    PersonalizeRig rig;
    fv::SettingsDialog& dialog = shown(rig);
    fv::SnippetsPanel& panel = snippetDraft(rig, QStringLiteral("my address"), QString());
    dialog.selectDestination(qs(dest::general));

    dialog.close();

    CHECK(rig.asked == QStringList{QStringLiteral(
                           "Unsaved snippet: Add the text to insert. Discard your changes to "
                           "this snippet?")});
    CHECK(dialog.isVisible());
    CHECK(dialog.currentDestination() == qs(dest::snippets));
    CHECK(panel.triggerEdit()->text() == QStringLiteral("my address"));
    CHECK(snippetTriggers(rig).empty());
    rig.answer = QMessageBox::Discard;
    dialog.close();
}

// from
// tests/test_personalize_fix_f4_unsaved_drafts.py::test_done_with_an_unfinished_draft_can_discard_it
TEST_CASE("test_done_with_an_unfinished_draft_can_discard_it",
          "[test_personalize_fix_f4_unsaved_drafts][widget]") {
    PersonalizeRig rig;
    fv::SettingsDialog& dialog = shown(rig);
    fv::TransformsPanel& panel = transformDraft(rig, QStringLiteral("Make it a haiku"), QString());
    rig.answer = QMessageBox::Discard;

    dialog.close();

    REQUIRE(rig.asked.size() == 1);
    CHECK(rig.asked.front().startsWith(QStringLiteral("Unsaved transform: ")));
    CHECK_FALSE(dialog.isVisible());
    CHECK_FALSE(panel.hasUnsavedChanges());
    CHECK_FALSE(hasTransform(rig, "Make it a haiku"));
}

// from tests/test_personalize_fix_f4_unsaved_drafts.py::test_closing_without_drafts_asks_nothing
TEST_CASE("test_closing_without_drafts_asks_nothing",
          "[test_personalize_fix_f4_unsaved_drafts][widget]") {
    PersonalizeRig rig;
    fv::SettingsDialog& dialog = shown(rig);
    dialog.selectDestination(qs(dest::snippets));
    rig.snippetEditor();
    dialog.selectDestination(qs(dest::commands));
    rig.transforms();

    dialog.close();

    CHECK(rig.asked.isEmpty());
    CHECK_FALSE(dialog.isVisible());
}
