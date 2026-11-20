// Write-up styles on the Walkthrough tab: saving the prompt as a style, a style saved elsewhere,
// the New style dialog, deleting a style. Ported from tests/test_walkthrough_styles.py (the tab's
// seven tests).
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>

#include "settings/store.h"
#include "settings/testing/memory_store.h"
#include "tab_support.h"
#include "ui_walkthrough/new_style_dialog.h"
#include "ui_widgets/no_wheel.h"
#include "walkthrough/styles.h"

namespace fvtest {
namespace {

using fv::Json;
namespace tab = fv::walkthrough_tab;
namespace wt = fv::walkthrough;

QString qs(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// _saved: every style to pick from, as Settings has them now.
std::vector<wt::WriteUpStyle> saved(const TabRig& rig) {
    return wt::load_styles(rig.saved());
}

QStringList names(TabRig& rig) {
    QStringList out;
    for (int i = 0; i < rig->presetCombo()->count(); ++i) {
        out << rig->presetCombo()->itemText(i);
    }
    return out;
}

// _choose_new_style: picks New style… with the dialog filled by `fill` (none: cancelled);
// returns how many dialogs were shown.
std::size_t chooseNewStyle(TabRig& rig, std::function<void(fv::NewStyleDialog&)> fill = {}) {
    rig.dialog = [fill](QDialog& shown) {
        auto& dialog = dynamic_cast<fv::NewStyleDialog&>(shown);
        if (!fill) {
            return int(QDialog::Rejected);
        }
        fill(dialog);
        dialog.save();
        return dialog.result();
    };
    Q_EMIT rig->presetCombo()->activated(rig->presetCombo()->findText(qs(tab::NEW_STYLE)));
    return rig.dialogs.size();
}

} // namespace

// from tests/test_walkthrough_styles.py::test_your_own_words_can_be_saved_as_a_style
TEST_CASE("your own words can be saved as a style", "[test_walkthrough_styles][widget]") {
    TabRig rig;
    QWidget* area = rig->promptArea();
    CHECK_FALSE(rig->saveStyleButton()->isVisibleTo(area));
    rig->promptEdit()->setPlainText(QStringLiteral("A checklist for a new teammate."));
    CHECK(rig->saveStyleButton()->isVisibleTo(area));
    rig.styleName = QStringLiteral("Onboarding");

    rig->saveStyleButton()->click();

    CHECK(rig->presetCombo()->currentText() == QStringLiteral("Onboarding"));
    CHECK(names(rig).contains(QStringLiteral("Onboarding")));
    CHECK_FALSE(rig->saveStyleButton()->isVisibleTo(area));
    CHECK(rig->deleteStyleButton()->isVisibleTo(area));
}

// from tests/test_walkthrough_styles.py::test_a_style_saved_elsewhere_shows_on_return
TEST_CASE("a style saved elsewhere shows on return", "[test_walkthrough_styles][widget]") {
    TabRig rig;
    rig.settings->mutate(
        [](Json& settings) { wt::save_style(settings, "From the storyboard", "Words."); });

    rig->loadSettings();

    const QStringList all = names(rig);
    REQUIRE(all.size() >= 3);
    // "" is the separator.
    CHECK(all.mid(all.size() - 3) ==
          QStringList{QStringLiteral("From the storyboard"), QString(), qs(tab::NEW_STYLE)});
}

// from tests/test_walkthrough_styles.py::test_a_style_can_be_made_from_the_menu_and_is_used_at_once
TEST_CASE("a style can be made from the menu and is used at once",
          "[test_walkthrough_styles][widget]") {
    TabRig rig;

    chooseNewStyle(rig, [](fv::NewStyleDialog& dialog) {
        dialog.nameEdit()->setText(QStringLiteral("Release notes"));
        dialog.promptEdit()->setPlainText(QStringLiteral("Short release-note bullets."));
    });

    CHECK(saved(rig).back().name == "Release notes");
    CHECK(rig->promptEdit()->toPlainText() == QStringLiteral("Short release-note bullets."));
    CHECK(rig->presetCombo()->currentText() == QStringLiteral("Release notes"));
    CHECK(rig.saved().at("walkthrough_write_up_instructions") ==
          Json("Short release-note bullets."));
    CHECK(rig->deleteStyleButton()->isVisibleTo(rig->promptArea()));
}

// from tests/test_walkthrough_styles.py::
//     test_cancelling_a_new_style_leaves_the_prompt_and_menu_as_they_were
TEST_CASE("cancelling a new style leaves the prompt and menu as they were",
          "[test_walkthrough_styles][widget]") {
    TabRig rig;
    const QString before = rig->promptEdit()->toPlainText();

    CHECK(chooseNewStyle(rig) == 1);

    CHECK(rig->promptEdit()->toPlainText() == before);
    CHECK(rig->presetCombo()->currentText() == qs(wt::default_style().name));
    CHECK(saved(rig).size() == 1 + wt::starter_styles().size());
}

// from tests/test_walkthrough_styles.py::test_a_new_style_that_cant_be_saved_says_why
TEST_CASE("a new style that cant be saved says why", "[test_walkthrough_styles][widget]") {
    fv::SettingsStore settings(std::make_unique<fv::MemorySettingsFile>());
    fv::NewStyleDialog dialog(settings);
    dialog.nameEdit()->setText(QStringLiteral("Notes"));
    dialog.save();
    CHECK(dialog.result() != QDialog::Accepted);
    CHECK(dialog.error()->text().contains(QStringLiteral("instructions")));
    CHECK_FALSE(dialog.saved().has_value());
    dialog.close();
}

// from tests/test_walkthrough_styles.py::test_deleting_a_style_keeps_its_words_as_your_own
TEST_CASE("deleting a style keeps its words as your own", "[test_walkthrough_styles][widget]") {
    const wt::WriteUpStyle bug = wt::starter_styles()[1];
    TabRig rig;
    Q_EMIT rig->presetCombo()->activated(rig->presetCombo()->findText(qs(bug.name)));
    rig.answer = QMessageBox::Yes;

    rig->deleteStyleButton()->click();

    CHECK(rig.questions == 1);
    CHECK_FALSE(names(rig).contains(qs(bug.name)));
    for (const auto& style : saved(rig)) {
        CHECK(style.name != bug.name);
    }
    CHECK(rig->promptEdit()->toPlainText() == qs(bug.instructions));
    CHECK(rig->presetCombo()->currentText() == qs(tab::OWN_PROMPT));
}

// from tests/test_walkthrough_styles.py::test_the_default_style_cant_be_deleted
TEST_CASE("the default style cant be deleted", "[test_walkthrough_styles][widget]") {
    TabRig rig;
    CHECK(rig->presetCombo()->currentText() == qs(wt::default_style().name));
    CHECK_FALSE(rig->deleteStyleButton()->isVisibleTo(rig->promptArea()));
}

// Every QInputDialog, QMessageBox and New style dialog is preceded by hiding the popover: a modal
// over an open popup would close it under the user's hand.
TEST_CASE("a dialog the prompt opens first puts its popover away", "[walkthrough_tab][widget]") {
    TabRig rig;
    rig->resize(1100, 760);
    rig->show();
    rig->editPrompt();
    REQUIRE(rig->promptPopover()->isVisible());
    rig->promptEdit()->setPlainText(QStringLiteral("Mine."));
    bool hiddenFirst = false;
    rig->setTextPrompt([&](QWidget*, const QString&, const QString&) -> std::optional<QString> {
        hiddenFirst = !rig->promptPopover()->isVisible();
        return std::nullopt;
    });

    rig->saveStyleButton()->click();

    CHECK(hiddenFirst);
    // Cancelled: nothing saved as a style.
    CHECK(saved(rig).size() == 1 + wt::starter_styles().size());
}

// A name save_style refuses is said in a warning, and nothing changes.
TEST_CASE("a style name that cant be saved is said in a warning", "[walkthrough_tab][widget]") {
    TabRig rig;
    rig->promptEdit()->setPlainText(QStringLiteral("Mine."));
    rig.styleName = QStringLiteral("walkthrough");

    rig->saveStyleButton()->click();

    REQUIRE(rig.warnings.size() == 1);
    CHECK(rig.warnings[0] ==
          QStringLiteral("\"Walkthrough\" is the built-in style. Choose another name."));
    CHECK(saved(rig).size() == 1 + wt::starter_styles().size());
}

} // namespace fvtest
