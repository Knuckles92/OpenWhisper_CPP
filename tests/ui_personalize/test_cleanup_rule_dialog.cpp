// The learned cleanup rule confirm/edit dialog and the cleanup prompt editor.
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTextEdit>

#include "ui_personalize/cleanup_prompt_dialog.h"
#include "ui_personalize/cleanup_rule_dialog.h"

namespace {

using fv::CleanupRuleDialog;

QStringList buttonLabels(const QDialog& dialog) {
    QStringList labels;
    for (const QPushButton* button : dialog.findChildren<QPushButton*>()) {
        labels << button->text();
    }
    return labels;
}

QStringList labelTexts(const QDialog& dialog) {
    QStringList texts;
    for (const QLabel* label : dialog.findChildren<QLabel*>()) {
        texts << label->text();
    }
    return texts;
}

bool anyStartsWith(const QStringList& texts, const QString& prefix) {
    for (const QString& text : texts) {
        if (text.startsWith(prefix)) {
            return true;
        }
    }
    return false;
}

bool anyContains(const QStringList& texts, const QString& part) {
    for (const QString& text : texts) {
        if (text.contains(part)) {
            return true;
        }
    }
    return false;
}

void clickButton(const QDialog& dialog, const QString& label) {
    for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
        if (button->text() == label) {
            button->click();
            return;
        }
    }
    FAIL("no button " << label.toStdString());
}

std::unique_ptr<CleanupRuleDialog> polished(bool dictated = false) {
    return std::make_unique<CleanupRuleDialog>(QStringLiteral("Always spell the name as Alex."),
                                               dictated
                                                   ? QStringLiteral("always spell my name alex")
                                                   : QStringLiteral("always spell my name Alex"),
                                               QString(), dictated);
}

} // namespace

// from
// tests/test_cleanup_rule_dialog.py::TestCleanupRuleDialogChoice::test_polish_success_offers_both_choices
TEST_CASE("test_polish_success_offers_both_choices", "[test_cleanup_rule_dialog][widget]") {
    const auto dialog = polished();
    const QStringList labels = buttonLabels(*dialog);
    CHECK(labels.contains(QStringLiteral("Use Polished (Recommended)")));
    CHECK(labels.contains(QStringLiteral("Use Exactly as Typed")));
    CHECK_FALSE(labels.contains(QStringLiteral("Save Rule")));
    CHECK(dialog->offersChoice());
    CHECK(dialog->ruleEdit()->toPlainText() == QStringLiteral("Always spell the name as Alex."));
}

// from
// tests/test_cleanup_rule_dialog.py::TestCleanupRuleDialogChoice::test_use_exactly_as_typed_accepts_original
TEST_CASE("test_use_exactly_as_typed_accepts_original", "[test_cleanup_rule_dialog][widget]") {
    const auto dialog = polished();
    clickButton(*dialog, QStringLiteral("Use Exactly as Typed"));
    CHECK(dialog->result() == QDialog::Accepted);
    CHECK(dialog->ruleText() == QStringLiteral("always spell my name Alex"));
}

// from
// tests/test_cleanup_rule_dialog.py::TestCleanupRuleDialogChoice::test_use_polished_keeps_edits
TEST_CASE("test_use_polished_keeps_edits", "[test_cleanup_rule_dialog][widget]") {
    const auto dialog = polished();
    dialog->ruleEdit()->setPlainText(QStringLiteral("Custom polished edit"));
    clickButton(*dialog, QStringLiteral("Use Polished (Recommended)"));
    CHECK(dialog->result() == QDialog::Accepted);
    CHECK(dialog->ruleText() == QStringLiteral("Custom polished edit"));
}

// from
// tests/test_cleanup_rule_dialog.py::TestCleanupRuleDialogChoice::test_polish_error_falls_back_to_single_save
TEST_CASE("test_polish_error_falls_back_to_single_save", "[test_cleanup_rule_dialog][widget]") {
    CleanupRuleDialog dialog(
        QStringLiteral("always spell my name Alex"), QStringLiteral("always spell my name Alex"),
        QStringLiteral("AI polish unavailable — your wording will be saved as written."));
    const QStringList labels = buttonLabels(dialog);
    CHECK(labels.contains(QStringLiteral("Save Rule")));
    CHECK_FALSE(labels.contains(QStringLiteral("Use Polished (Recommended)")));
    CHECK_FALSE(dialog.offersChoice());
}

// from
// tests/test_cleanup_rule_dialog.py::TestCleanupRuleDialogChoice::test_identical_texts_skip_choice
TEST_CASE("test_identical_texts_skip_choice", "[test_cleanup_rule_dialog][widget]") {
    CleanupRuleDialog dialog(QStringLiteral("Keep acronyms uppercase."),
                             QStringLiteral("Keep acronyms uppercase."));
    CHECK_FALSE(dialog.offersChoice());
    CHECK(buttonLabels(dialog).contains(QStringLiteral("Save Rule")));
}

// from
// tests/test_cleanup_rule_dialog.py::TestCleanupRuleDialogChoice::test_typed_original_is_quoted_as_typed
TEST_CASE("test_typed_original_is_quoted_as_typed", "[test_cleanup_rule_dialog][widget]") {
    const auto dialog = polished();
    const QStringList texts = labelTexts(*dialog);
    CHECK(anyStartsWith(texts, QStringLiteral("You typed")));
    CHECK(anyContains(texts, QStringLiteral("exactly what you typed")));
}

// from
// tests/test_cleanup_rule_dialog.py::TestCleanupRuleDialogChoice::test_dictated_original_is_quoted_as_said
TEST_CASE("test_dictated_original_is_quoted_as_said", "[test_cleanup_rule_dialog][widget]") {
    const auto dialog = polished(true);
    const QStringList labels = buttonLabels(*dialog);
    CHECK(labels.contains(QStringLiteral("Use What I Said")));
    CHECK_FALSE(labels.contains(QStringLiteral("Use Exactly as Typed")));
    const QStringList texts = labelTexts(*dialog);
    CHECK(anyStartsWith(texts, QStringLiteral("You said")));
    CHECK(anyContains(texts, QStringLiteral("your words as transcribed")));
    CHECK_FALSE(anyContains(texts, QStringLiteral("typed")));
    clickButton(*dialog, QStringLiteral("Use What I Said"));
    CHECK(dialog->ruleText() == QStringLiteral("always spell my name alex"));
}

// from
// tests/test_cleanup_rule_dialog.py::TestCleanupRuleDialogChoice::test_edit_mode_has_no_choice
TEST_CASE("test_edit_mode_has_no_choice", "[test_cleanup_rule_dialog][widget]") {
    CleanupRuleDialog dialog(QStringLiteral("Existing rule text"));
    CHECK_FALSE(dialog.offersChoice());
    CHECK(buttonLabels(dialog).contains(QStringLiteral("Save Rule")));
    CHECK(dialog.windowTitle() == QStringLiteral("Edit Learned Rule"));
}

TEST_CASE("the review dialog is titled and sized as the choice needs", "[widget]") {
    const auto review = polished();
    CHECK(review->windowTitle() == QStringLiteral("Confirm Learned Rule"));
    CHECK(review->minimumSize() == QSize(620, 330));
    CHECK(review->size() == QSize(660, 410));
    CleanupRuleDialog edit(QStringLiteral("Existing rule text"));
    CHECK(edit.minimumSize() == QSize(620, 300));
    CHECK(edit.size() == QSize(660, 360));
    CHECK(edit.objectName() == QStringLiteral("cleanupRuleDialog"));
}

TEST_CASE("the choice needs both texts, which must differ beyond case", "[widget]") {
    // casefold, not lower: the German sharp s folds to "ss".
    CleanupRuleDialog folded(QStringLiteral("Straße nennen"), QStringLiteral("STRASSE NENNEN"));
    CHECK_FALSE(folded.offersChoice());
    // An original with only blanks has nothing to offer.
    CleanupRuleDialog blank(QStringLiteral("A rule."), QStringLiteral("   "));
    CHECK_FALSE(blank.offersChoice());
    // A polished text that came back empty has nothing to offer either.
    CleanupRuleDialog empty(QStringLiteral("  "), QStringLiteral("said this"));
    CHECK_FALSE(empty.offersChoice());
    // Different wording is a choice.
    CleanupRuleDialog other(QStringLiteral("A different rule."), QStringLiteral("said this"));
    CHECK(other.offersChoice());
}

TEST_CASE("Use Polished falls back to the polished text when the editor was emptied", "[widget]") {
    const auto dialog = polished();
    dialog->ruleEdit()->setPlainText(QStringLiteral("  \n "));
    clickButton(*dialog, QStringLiteral("Use Polished (Recommended)"));
    CHECK(dialog->result() == QDialog::Accepted);
    CHECK(dialog->ruleText() == QStringLiteral("Always spell the name as Alex."));
}

TEST_CASE("the rule text is trimmed of every kind of blank", "[widget]") {
    CleanupRuleDialog dialog(QStringLiteral("x"));
    dialog.ruleEdit()->setPlainText(QStringLiteral(" \t Keep this.\n　"));
    CHECK(dialog.ruleText() == QStringLiteral("Keep this."));
}

TEST_CASE("Cancel rejects and the notice replaces the choice", "[widget]") {
    CleanupRuleDialog dialog(QStringLiteral("A rule."), QStringLiteral("said this"),
                             QStringLiteral("AI polish unavailable"), true);
    CHECK(anyContains(labelTexts(dialog), QStringLiteral("AI polish unavailable")));
    CHECK(anyStartsWith(labelTexts(dialog), QStringLiteral("You said")));
    clickButton(dialog, QStringLiteral("Cancel"));
    CHECK(dialog.result() == QDialog::Rejected);
}

TEST_CASE("an edit dialog says nothing of an original", "[widget]") {
    CleanupRuleDialog dialog(QStringLiteral("Existing rule text"));
    const QStringList texts = labelTexts(dialog);
    CHECK_FALSE(anyStartsWith(texts, QStringLiteral("You ")));
    CHECK(texts.contains(QStringLiteral("Aa")));
    CHECK(texts.contains(QStringLiteral("Edit learned rule")));
}

TEST_CASE("the prompt editor holds the prompt untrimmed", "[widget]") {
    fv::CleanupPromptDialog dialog(QStringLiteral("  Clean it up.\n"));
    CHECK(dialog.windowTitle() == QStringLiteral("Edit Cleanup Prompt"));
    CHECK(dialog.minimumSize() == QSize(520, 420));
    CHECK(dialog.size() == QSize(560, 480));
    CHECK(dialog.promptText() == QStringLiteral("  Clean it up.\n"));
    CHECK(dialog.promptEdit()->placeholderText() == QStringLiteral("Enter cleanup instructions…"));
    CHECK_FALSE(dialog.promptEdit()->acceptRichText());
    CHECK(buttonLabels(dialog) ==
          QStringList({QStringLiteral("Cancel"), QStringLiteral("Use Prompt")}));
    clickButton(dialog, QStringLiteral("Use Prompt"));
    CHECK(dialog.result() == QDialog::Accepted);
}

TEST_CASE("the prompt editor's Cancel rejects", "[widget]") {
    fv::CleanupPromptDialog dialog{QString()};
    clickButton(dialog, QStringLiteral("Cancel"));
    CHECK(dialog.result() == QDialog::Rejected);
}
