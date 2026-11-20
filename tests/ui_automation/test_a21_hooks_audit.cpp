// Checks the M10 sweep (auditor AU7) added for Hooks rows of
// docs/port/map/a21-actions-automation-ui.md whose words no ported test pinned.

#include <catch2/catch_test_macros.hpp>

#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>

#include "base/identity.h"
#include "hooks_support.h"
#include "ui_automation/hooks_editors.h"
#include "ui_automation/hooks_page.h"
#include "ui_automation/hooks_panel.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/empty_state.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/wrapped_label.h"

using namespace fvtest;

namespace {

const char* const kTags = "[widget]";

QString product() {
    return qs(fv::identity().product);
}

void checkSection(fv::HookSection& section, const QString& noun, const QString& emptyTitle,
                  const QString& emptyHint, const QString& namePlaceholder) {
    INFO(noun.toStdString());
    CHECK(section.emptyState()->titleLabel()->text() == emptyTitle);
    CHECK(section.emptyState()->hintLabel()->text() == emptyHint);
    CHECK(section.addButton()->text() == QStringLiteral("Add %1").arg(noun));
    CHECK(section.addButton()->accessibleName() == QStringLiteral("Add a %1").arg(noun));
    section.addButton()->click();
    fv::HookEditor& editor = *section.editor();
    CHECK(editor.nameEdit()->placeholderText() == namePlaceholder);
    CHECK(editor.nameEdit()->maxLength() == 80);
    CHECK(editor.cancelButton()->text() == QStringLiteral("Cancel"));
    CHECK(editor.cancelButton()->toolTip() == QStringLiteral("Close without saving"));
    CHECK(editor.saveButton()->text() == QStringLiteral("Save %1").arg(noun));
    CHECK(editor.deleteButton()->accessibleName() == QStringLiteral("Delete this %1").arg(noun));
}

} // namespace

// A21-39, A21-67: the page's words.
TEST_CASE("a21 audit: the Hooks page says what it is for", kTags) {
    CHECK(fv::hooks_page::TITLE == "Hooks");
    CHECK(fv::hooks_page::SUBTITLE == "Tell your own tools when you dictate, and let them change "
                                      "the words before they're pasted.");
    const auto& fields = fv::hooks_page::search_fields();
    REQUIRE(fields.size() == 3);
    CHECK(fields[0].title == "Webhooks");
    CHECK(fields[0].description ==
          "Send events such as finished dictations to a web address, signed so it can check them.");
    CHECK(fields[1].title == "Programs");
    CHECK(fields[1].description == "Run a program on this computer when something happens, with "
                                   "the event as JSON on its input.");
    CHECK(fields[2].title == "Text filters");
    CHECK(fields[2].description ==
          "Let a web address or a program change your words, before or after AI cleanup.");
}

// A21-41, A21-42, A21-73: empty states, Add, and the editor's common words per kind.
TEST_CASE("a21 audit: each hook list invites its first item and names its editor", kTags) {
    HooksRig rig;
    auto webhooks = rig.section<fv::WebhookSection>();
    checkSection(*webhooks, QStringLiteral("webhook"), QStringLiteral("No webhooks yet"),
                 QStringLiteral("Add one to send finished dictations to a web address."),
                 QStringLiteral("e.g. Home automation"));
    auto programs = rig.section<fv::ProgramSection>();
    checkSection(*programs, QStringLiteral("program"), QStringLiteral("No programs yet"),
                 QStringLiteral("Add one to run your own script when you dictate."),
                 QStringLiteral("e.g. Log my dictations"));
    auto filters = rig.section<fv::FilterSection>();
    checkSection(*filters, QStringLiteral("filter"), QStringLiteral("No text filters yet"),
                 QStringLiteral("Add one to let your own tool fix words before they're pasted."),
                 QStringLiteral("e.g. Fix product names"));
}

// A21-43, A21-48, A21-72: the webhook editor's words and its questions.
TEST_CASE("a21 audit: the webhook editor's words, questions and tooltips", kTags) {
    HooksRig rig;
    auto section = rig.section<fv::WebhookSection>();
    section->addButton()->click();
    fv::WebhookEditor& editor = *section->webhookEditor();
    CHECK(editor.urlEdit()->placeholderText() ==
          QStringLiteral("https://example.com/%1").arg(product().toLower()));
    CHECK(editor.urlEdit()->maxLength() == 2000);
    editor.urlEdit()->setText(QStringLiteral("http://example.com/in"));
    CHECK(editor.urlNote()->text() ==
          QStringLiteral("Sent unencrypted. That's fine on your own network; use https:// for "
                         "anything farther away."));
    editor.nameEdit()->setText(QStringLiteral("Home"));
    editor.saveButton()->click();
    REQUIRE(rig.savedWebhooks().size() == 1);
    CHECK(editor.copySecretButton()->text() == QStringLiteral("Copy secret"));
    CHECK(editor.rotateSecretButton()->text() == QStringLiteral("Make a new one"));
    CHECK(editor.rotateSecretButton()->toolTip() ==
          QStringLiteral("Replace the secret; receivers need the new one"));
    CHECK(editor.testButton()->text() == QStringLiteral("Send test"));
    CHECK(editor.testButton()->toolTip() ==
          QStringLiteral("Send a test event to this address now"));

    rig.answers.push_back(QMessageBox::Cancel);
    editor.rotateSecretButton()->click();
    REQUIRE(rig.questions.size() == 1);
    CHECK(rig.questions.back().first == QStringLiteral("Make a new signing secret?"));
    CHECK(rig.questions.back().second ==
          QStringLiteral("Your receiver will reject deliveries until you give it the new secret."));

    editor.urlEdit()->setText(QStringLiteral("https://example.org/in"));
    rig.answers.push_back(QMessageBox::Cancel);
    section->addButton()->click();
    REQUIRE(rig.questions.size() == 2);
    CHECK(rig.questions.back().first == QStringLiteral("Unsaved webhook"));
    CHECK(rig.questions.back().second == QStringLiteral("Save your changes to this webhook?"));
    editor.urlEdit()->setText(QStringLiteral("http://example.com/in"));

    rig.answers.push_back(QMessageBox::Cancel);
    editor.deleteButton()->click();
    REQUIRE(rig.questions.size() == 3);
    CHECK(rig.questions.back().first == QStringLiteral("Delete webhook?"));
    CHECK(rig.questions.back().second ==
          QStringLiteral(u"Delete “Home”? %1 stops sending to it and forgets its signing secret.")
              .arg(product()));
    CHECK(rig.savedWebhooks().size() == 1);
}

// A21-44, A21-48, A21-73: the program editor's words.
TEST_CASE("a21 audit: the program editor's words", kTags) {
    HooksRig rig;
    auto section = rig.section<fv::ProgramSection>();
    section->addButton()->click();
    fv::ProgramEditor& editor = *section->programEditor();
    CHECK(editor.programEdit()->placeholderText() ==
          QStringLiteral("Choose a program on this computer"));
    CHECK_FALSE(editor.programEdit()->isReadOnly());
    CHECK(editor.chooseButton()->text() == QStringLiteral(u"Choose…"));
    CHECK(editor.chooseButton()->accessibleName() == QStringLiteral("Choose a program"));
    CHECK(editor.argsEdit()->placeholderText() == QStringLiteral("One per line, e.g. --quiet"));
    CHECK(editor.argsEdit()->accessibleName() == QStringLiteral("Program arguments, one per line"));
    CHECK(editor.argsEdit()->tabChangesFocus());
    CHECK(editor.timeout()->minimum() == 1);
    CHECK(editor.timeout()->maximum() == 300);
    CHECK(editor.timeout()->suffix() == QStringLiteral(" s"));
    CHECK(editor.testButton()->text() == QStringLiteral("Run test"));
    CHECK(editor.testButton()->toolTip() ==
          QStringLiteral("Run the program now with a test event"));
}

// A21-45: the filter editor's kinds, stages, time limit and reply rules.
TEST_CASE("a21 audit: the filter editor's words and time limit", kTags) {
    HooksRig rig;
    auto section = rig.section<fv::FilterSection>();
    section->addButton()->click();
    fv::FilterEditor& editor = *section->filterEditor();
    REQUIRE(editor.kindBar()->buttons().size() == 2);
    CHECK(editor.kindBar()->buttons()[0]->titleLabel()->text() == QStringLiteral("Web address"));
    CHECK(editor.kindBar()->buttons()[1]->titleLabel()->text() == QStringLiteral("Program"));
    CHECK(editor.urlEdit()->placeholderText() == QStringLiteral("http://127.0.0.1:8080/filter"));
    REQUIRE(editor.stageBar()->buttons().size() == 2);
    CHECK(editor.stageBar()->buttons()[0]->titleLabel()->text() ==
          QStringLiteral("Before AI cleanup"));
    CHECK(editor.stageBar()->buttons()[0]->detailLabel()->text() ==
          QStringLiteral("Words as heard"));
    CHECK(editor.stageBar()->buttons()[1]->titleLabel()->text() ==
          QStringLiteral("After AI cleanup"));
    CHECK(editor.stageBar()->buttons()[1]->detailLabel()->text() == QStringLiteral("Final words"));
    CHECK(editor.timeout()->minimum() == 100);
    CHECK(editor.timeout()->maximum() == 5000);
    CHECK(editor.timeout()->singleStep() == 100);
    CHECK(editor.timeout()->suffix() == QStringLiteral(" ms"));
    CHECK(editor.timeout()->value() == 1500);
    CHECK(editor.replyHint()->text() ==
          QStringLiteral(u"%1 posts the words as JSON. Reply {\"text\": \"…\"} to change them, {} "
                         u"to keep them, or {\"cancel\": true} to paste nothing. A slow or odd "
                         u"reply keeps your words.")
              .arg(product()));
    editor.kindBar()->buttons()[1]->click();
    CHECK(editor.replyHint()->text().startsWith(
        QStringLiteral("The words arrive as JSON on the program's input.")));
}

// A21-46: the leases note and its link.
TEST_CASE("a21 audit: the filter leases note points at API & MCP", kTags) {
    HooksRig rig;
    auto section = rig.section<fv::FilterSection>();
    fv::LeaseList& leases = *section->leases();
    leases.refresh();
    CHECK(leases.note()->text() ==
          QStringLiteral("A program on this computer can add a filter for a minute at a time "
                         "through the live API, once you allow Change dictated text in API & "
                         "MCP."));
    CHECK(leases.openApi()->text() == QStringLiteral("Open API && MCP"));
}
