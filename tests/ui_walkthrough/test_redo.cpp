// Redoing a write-up from the storyboard, with a saved style or a new prompt, and the "Redo with
// a new prompt" dialog (ui_qt/dialogs/write_up_prompt_dialog.py), from
// tests/test_walkthrough_styles.py. Its dialog never shows: the review's dialog runner fills and
// answers it as the Python tests' patched exec does.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include <QAction>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>

#include "review_support.h"
#include "settings/keys.h"
#include "ui_walkthrough/write_up_prompt_dialog.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "walkthrough/styles.h"

namespace fvtest {
namespace {

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

std::vector<wt::WriteUpStyle> saved(const fv::SettingsStore& settings) {
    return wt::load_styles(*settings.snapshot());
}

QString q(const std::string& text) {
    return QString::fromStdString(text);
}

// OpenWhisper's review and redone fixtures: a storyboard on the capture whose write-ups come
// from a stand-in agent that answers every request.
struct Redo {
    Capture capture;
    Review review;

    Redo() {
        review.agent.reply = "# Redone";
        review->showFolder(capture.folder);
    }

    // The words each write-up was asked to follow, from the agent's message.
    bool askedFor(std::string_view words) const {
        return !review.agent.calls.empty() &&
               contains(review.agent.calls.back().user_prompt, words);
    }
};

struct Dialogs {
    fv::SettingsStore settings{std::make_unique<fv::MemorySettingsFile>()};
    std::vector<std::unique_ptr<fv::WriteUpPromptDialog>> made;

    fv::WriteUpPromptDialog& make(const QString& prompt, bool written = true) {
        made.push_back(std::make_unique<fv::WriteUpPromptDialog>(settings, prompt, written));
        return *made.back();
    }
};

} // namespace

// from tests/test_walkthrough_styles.py::
//     test_redo_as_lists_every_style_and_writes_it_up_with_the_one_picked
TEST_CASE("redo as lists every style and writes it up with the one picked",
          "[test_walkthrough_styles][widget]") {
    Redo redo;
    redo.review.settings->mutate(
        [](fv::Json& settings) { wt::save_style(settings, "Release notes", "Bullets."); });
    Q_EMIT redo.review->styleMenu()->aboutToShow();
    const auto actions = redo.review->styleMenu()->actions();
    std::vector<QString> names;
    for (QAction* action : actions) {
        names.push_back(action->text());
    }
    std::vector<QString> expected;
    for (const auto& style : saved(*redo.review.settings)) {
        expected.push_back(q(style.name));
    }
    CHECK(names == expected);
    CHECK(actions.back()->text() == QStringLiteral("Release notes"));

    for (QAction* action : actions) {
        if (action->text() == QStringLiteral("Bug report")) {
            action->trigger();
        }
    }
    redo.review.wait();

    REQUIRE(redo.review.agent.calls.size() == 1);
    CHECK(redo.askedFor(wt::starter_styles()[1].instructions));
}

// from tests/test_walkthrough_styles.py::test_the_menu_says_write_up_until_there_is_one
TEST_CASE("the menu says write up until there is one", "[test_walkthrough_styles][widget]") {
    Redo redo;
    CHECK(redo.review->styleMenu()->title() == QStringLiteral("Write up as"));
    CHECK(redo.review->promptAction()->text() == QStringLiteral(u"Write up with a new prompt…"));
}

// from
// tests/test_walkthrough_styles.py::test_a_new_prompt_writes_it_up_once_without_changing_settings
TEST_CASE("a new prompt writes it up once without changing settings",
          "[test_walkthrough_styles][widget]") {
    Redo redo;
    redo.review.settings->save_setting(fv::keys::walkthrough_write_up_instructions.key(),
                                       "Usual words.");
    std::vector<QString> opened;
    redo.review->setDialogRunner([&opened](QDialog& shown) {
        auto& dialog = dynamic_cast<fv::WriteUpPromptDialog&>(shown);
        opened.push_back(dialog.text());
        dialog.promptEdit()->setPlainText(QStringLiteral("A one-paragraph summary."));
        dialog.go();
        return dialog.result();
    });

    redo.review->promptAction()->trigger();
    redo.review.wait();

    // It starts from the usual prompt.
    CHECK(opened == std::vector<QString>{QStringLiteral("Usual words.")});
    REQUIRE(redo.review.agent.calls.size() == 1);
    CHECK(redo.askedFor("A one-paragraph summary."));
    CHECK(redo.review.settings->get(fv::keys::walkthrough_write_up_instructions) == "Usual words.");
}

// from tests/test_walkthrough_styles.py::test_canceling_the_new_prompt_writes_nothing_up
TEST_CASE("canceling the new prompt writes nothing up", "[test_walkthrough_styles][widget]") {
    Redo redo;
    redo.review->setDialogRunner([](QDialog&) { return 0; });
    redo.review->promptAction()->trigger();
    CHECK_FALSE(redo.review->isBusy());
    redo.review.wait();
    CHECK(redo.review.agent.calls.empty());
}

// from tests/test_walkthrough_styles.py::test_unsaved_edits_hold_a_redo_with_a_new_prompt
TEST_CASE("unsaved edits hold a redo with a new prompt", "[test_walkthrough_styles][widget]") {
    Redo redo;
    int opened = 0;
    redo.review->setDialogRunner([&opened](QDialog&) {
        ++opened;
        return 0;
    });
    redo.review->rows()[0]->editor()->setPlainText(QStringLiteral("corrected"));
    redo.review->promptAction()->trigger();
    CHECK(opened == 0);
    CHECK(redo.review->notice()->text().contains(QStringLiteral("Save")));
}

// from tests/test_walkthrough_styles.py::test_the_dialog_starts_on_the_style_its_words_are
TEST_CASE("the dialog starts on the style its words are", "[test_walkthrough_styles][widget]") {
    Dialogs dialogs;
    auto& dialog = dialogs.make(q(wt::starter_styles()[2].instructions));
    const QString own = QString::fromLatin1(fv::WriteUpPromptDialog::kOwnWords);
    CHECK(dialog.styleCombo()->currentText() == q(wt::starter_styles()[2].name));
    CHECK(dialog.styleCombo()->findText(own) < 0);

    dialog.promptEdit()->setPlainText(QStringLiteral("My own."));
    CHECK(dialog.styleCombo()->currentText() == own);

    const int index = dialog.styleCombo()->findText(QStringLiteral("Bug report"));
    Q_EMIT dialog.styleCombo()->activated(index);
    CHECK(dialog.text() == q(wt::starter_styles()[1].instructions));
    CHECK(dialog.styleCombo()->currentText() == QStringLiteral("Bug report"));
}

// from tests/test_walkthrough_styles.py::test_an_empty_prompt_isnt_sent
TEST_CASE("an empty prompt isn't sent", "[test_walkthrough_styles][widget]") {
    Dialogs dialogs;
    auto& dialog = dialogs.make(QStringLiteral("  "));
    dialog.go();
    CHECK(dialog.result() != QDialog::Accepted);
    CHECK(dialog.errorLabel()->text().contains(QStringLiteral("Type what you want")));
}

// from tests/test_walkthrough_styles.py::test_naming_the_prompt_keeps_it_as_a_style
TEST_CASE("naming the prompt keeps it as a style", "[test_walkthrough_styles][widget]") {
    Dialogs dialogs;
    auto& dialog = dialogs.make(QStringLiteral("Short changelog bullets."));
    dialog.nameEdit()->setText(QStringLiteral("Changelog"));
    dialog.go();
    CHECK(dialog.result() == QDialog::Accepted);
    CHECK(saved(dialogs.settings).back().name == "Changelog");
    CHECK(saved(dialogs.settings).back().instructions == "Short changelog bullets.");
}

// from tests/test_walkthrough_styles.py::test_a_name_that_cant_be_saved_keeps_the_dialog_open
TEST_CASE("a name that can't be saved keeps the dialog open", "[test_walkthrough_styles][widget]") {
    Dialogs dialogs;
    auto& dialog = dialogs.make(QStringLiteral("Words."));
    dialog.nameEdit()->setText(QStringLiteral("Walkthrough"));
    dialog.go();
    CHECK(dialog.result() != QDialog::Accepted);
    CHECK(dialog.errorLabel()->text().contains(QStringLiteral("built-in")));
    CHECK(saved(dialogs.settings).size() == 1 + wt::starter_styles().size());
}

// from tests/test_walkthrough_styles.py::test_the_dialog_says_write_up_until_there_is_one
TEST_CASE("the dialog says write up until there is one", "[test_walkthrough_styles][widget]") {
    Dialogs dialogs;
    CHECK(dialogs.make(QStringLiteral("Words."), false).goButton()->text() ==
          QStringLiteral("Write it up"));
}

// Not in OpenWhisper's tests: the dialog's words and limits (A23-85).
TEST_CASE("the prompt dialog says what it does", "[walkthrough_review][widget]") {
    Dialogs dialogs;
    auto& redo = dialogs.make(QStringLiteral("Words."));
    CHECK(redo.windowTitle() == QStringLiteral("Redo write-up"));
    CHECK(redo.goButton()->text() == QStringLiteral("Redo write-up"));
    CHECK(redo.minimumSize() == QSize(520, 440));
    CHECK(redo.size() == QSize(580, 500));
    CHECK(redo.nameEdit()->maxLength() == 60);
    CHECK(redo.styleCombo()->accessibleName() == QStringLiteral("Write-up style"));
    CHECK(redo.promptEdit()->accessibleName() == QStringLiteral("Write-up prompt"));
    CHECK(redo.errorLabel()->isHidden());
    auto& first = dialogs.make(QStringLiteral("Words."), false);
    CHECK(first.windowTitle() == QStringLiteral("Write up with AI"));
    // An empty prompt is the user's own words, not the default style.
    auto& empty = dialogs.make(QString());
    CHECK(empty.styleCombo()->currentText() ==
          QString::fromLatin1(fv::WriteUpPromptDialog::kOwnWords));
}

} // namespace fvtest
