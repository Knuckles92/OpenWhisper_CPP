// ActionList and ActionCard (ui_qt/widgets/action_list.py): the card's chips, links and
// switch, two columns from 640 px at the font scale with rows of equal height, Tab in reading
// order, and the early return on an unchanged list. The Python page tests that exercise the list
// are the Actions page's.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

#include <QApplication>
#include <QGridLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>

#include "automation_support.h"
#include "ui_automation/action_list.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/empty_state.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::ActionCard;
using fv::ActionList;

std::pair<int, int> cellOf(ActionList& list, QWidget* card) {
    int row = 0;
    int column = 0;
    int rowSpan = 0;
    int columnSpan = 0;
    list.grid()->getItemPosition(list.grid()->indexOf(card), &row, &column, &rowSpan, &columnSpan);
    return {row, column};
}

void showAt(QWidget& widget, int width) {
    widget.resize(width, 600);
    widget.show();
    QApplication::processEvents();
}

} // namespace

TEST_CASE("an action card shows what it does and how to start it", "[widget]") {
    fvtest::AppTheme theme;
    ActionCard card(fvtest::askClaude());
    CHECK(card.titleLabel()->text() == QStringLiteral("Ask Claude"));
    CHECK(card.descriptionLabel()->text() ==
          QStringLiteral("Opens a new Claude chat with your words"));
    CHECK(card.shortcutChip()->text() == QStringLiteral("Ctrl+Alt+C"));
    CHECK(card.shortcutChip()->property(fv::ui::prop::tone).toString() == QStringLiteral("key"));
    CHECK(card.shortcutChip()->accessibleName() == QStringLiteral("Shortcut Ctrl+Alt+C"));
    REQUIRE(card.spokenChip() != nullptr);
    CHECK(card.spokenChip()->text() == QStringLiteral(u"Say “Claude”"));
    CHECK(card.spokenChip()->property(fv::ui::prop::tone).toString() == QStringLiteral("voice"));
    CHECK(card.switchButton()->isChecked());
    CHECK(card.switchButton()->accessibleName() == QStringLiteral("Ask Claude on"));
    CHECK(card.editLink()->accessibleName() == QStringLiteral("Edit Ask Claude"));
    CHECK(card.editLink()->toolTip() == QStringLiteral("Change Ask Claude"));
    CHECK(card.deleteLink()->accessibleName() == QStringLiteral("Delete Ask Claude"));
    CHECK(card.deleteLink()->property(fv::ui::prop::tone).toString() == QStringLiteral("danger"));
    CHECK(card.property(fv::ui::prop::off).toBool() == false);
    CHECK(card.property(fv::ui::prop::tileId).toString() == QStringLiteral("actionCard"));
    CHECK(card.iconLabel()->property(fv::ui::prop::iconTone).toString() == QStringLiteral("blue"));
    CHECK(card.iconLabel()->size() == QSize(32, 32));
}

TEST_CASE("an action card without a shortcut or spoken name says so", "[widget]") {
    ActionCard card(fvtest::homeAutomation());
    CHECK(card.shortcutChip()->text() == QStringLiteral("No shortcut"));
    CHECK(card.shortcutChip()->property(fv::ui::prop::tone).toString() == QStringLiteral("muted"));
    CHECK(card.shortcutChip()->accessibleName() == QStringLiteral("No shortcut"));
    CHECK(card.spokenChip() == nullptr);
    CHECK_FALSE(card.switchButton()->isChecked());
    CHECK(card.property(fv::ui::prop::off).toBool());
}

TEST_CASE("an action card's switch, Edit and Delete name the action", "[widget]") {
    ActionCard card(fvtest::askClaude());
    QSignalSpy toggled(&card, &ActionCard::enabledToggled);
    QSignalSpy edit(&card, &ActionCard::editRequested);
    QSignalSpy remove(&card, &ActionCard::deleteRequested);
    card.switchButton()->toggle();
    card.editLink()->click();
    card.deleteLink()->click();
    REQUIRE(toggled.count() == 1);
    CHECK(toggled.at(0).at(0).toString() == QStringLiteral("ask-claude"));
    CHECK(toggled.at(0).at(1).toBool() == false);
    REQUIRE(edit.count() == 1);
    CHECK(edit.at(0).at(0).toString() == QStringLiteral("ask-claude"));
    REQUIRE(remove.count() == 1);
    CHECK(remove.at(0).at(0).toString() == QStringLiteral("ask-claude"));

    // Shown from saved state: no signal, the dimmed look follows.
    card.setEnabledShown(false);
    CHECK(toggled.count() == 1);
    CHECK(card.property(fv::ui::prop::off).toBool());
    card.setEnabledShown(true);
    CHECK(card.switchButton()->isChecked());
    CHECK(card.property(fv::ui::prop::off).toBool() == false);
}

TEST_CASE("an empty action list invites the first action", "[widget]") {
    ActionList list;
    showAt(list, 700);
    CHECK(list.cards().empty());
    CHECK(list.emptyState()->isVisible());
    CHECK_FALSE(list.gridBox()->isVisible());
    CHECK(list.emptyState()->text().contains(QStringLiteral("No actions yet")));
    QSignalSpy add(&list, &ActionList::addRequested);
    list.emptyAddButton()->click();
    CHECK(add.count() == 1);
}

TEST_CASE("action cards sit two to a row from 640 px", "[widget]") {
    fvtest::AppTheme theme;
    ActionList list;
    list.setActions(fvtest::fixedActions());
    showAt(list, 639);
    CHECK(list.columns() == 1);
    CHECK(cellOf(list, list.cards()[1]) == std::pair{1, 0});
    showAt(list, 640);
    CHECK(list.columns() == 2);
    CHECK(cellOf(list, list.cards()[0]) == std::pair{0, 0});
    CHECK(cellOf(list, list.cards()[1]) == std::pair{0, 1});
    CHECK(cellOf(list, list.cards()[2]) == std::pair{1, 0});
    CHECK_FALSE(list.emptyState()->isVisible());
}

TEST_CASE("the two-column width grows with the font scale", "[widget]") {
    fvtest::AppTheme theme(QStringLiteral("dark"), 130);
    ActionList list;
    list.setActions(fvtest::fixedActions());
    // 640 x 1.3 = 832.
    showAt(list, 831);
    CHECK(list.columns() == 1);
    showAt(list, 832);
    CHECK(list.columns() == 2);
}

TEST_CASE("a single action card takes the whole row", "[widget]") {
    ActionList list;
    list.setActions({fvtest::askClaude()});
    showAt(list, 900);
    CHECK(list.columns() == 1);
}

TEST_CASE("cards in a row share its height with chips and links on the bottom line", "[widget]") {
    fvtest::AppTheme theme;
    auto wordy = fvtest::logIt();
    wordy.name = "Log every word I say into a very long file name with plenty of words";
    ActionList list;
    list.setActions({fvtest::askClaude(), wordy});
    showAt(list, 660);
    REQUIRE(list.columns() == 2);
    ActionCard* left = list.cards()[0];
    ActionCard* right = list.cards()[1];
    CHECK(left->height() == right->height());
    CHECK(left->titleLabel()->height() < right->titleLabel()->height());
    const auto bottomOf = [](ActionCard* card) {
        return card->editLink()->mapTo(card, QPoint(0, card->editLink()->height())).y();
    };
    CHECK(bottomOf(left) == bottomOf(right));
}

TEST_CASE("tab reaches the cards in reading order after the empty state's button", "[widget]") {
    ActionList list;
    list.setActions(fvtest::fixedActions());
    showAt(list, 700);
    std::vector<QWidget*> expected;
    for (ActionCard* card : list.cards()) {
        for (QWidget* widget : card->focusOrder()) {
            expected.push_back(widget);
        }
    }
    QWidget* at = list.emptyAddButton();
    for (QWidget* widget : expected) {
        at = at->nextInFocusChain();
        while (at->focusPolicy() == Qt::NoFocus) {
            at = at->nextInFocusChain();
        }
        CHECK(at == widget);
    }
    CHECK(expected.size() == 9);
    CHECK(expected[0] == list.cards()[0]->switchButton());
    CHECK(expected[1] == list.cards()[0]->editLink());
    CHECK(expected[2] == list.cards()[0]->deleteLink());
}

TEST_CASE("an unchanged action list keeps its cards", "[widget]") {
    ActionList list;
    list.setActions(fvtest::fixedActions());
    showAt(list, 700);
    const QPointer<ActionCard> first = list.cards()[0];
    list.setActions(fvtest::fixedActions());
    QApplication::processEvents();
    REQUIRE_FALSE(first.isNull());
    CHECK(list.cards()[0] == first.data());

    // Any change rebuilds them; the old ones are deleted later.
    auto changed = fvtest::fixedActions();
    changed[0].enabled = false;
    list.setActions(changed);
    CHECK(list.cards()[0] != first.data());
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(first.isNull());
    CHECK(list.cards()[0]->property(fv::ui::prop::off).toBool());
}

TEST_CASE("the list forwards each card's requests", "[widget]") {
    ActionList list;
    list.setActions(fvtest::fixedActions());
    QSignalSpy edit(&list, &ActionList::editRequested);
    QSignalSpy remove(&list, &ActionList::deleteRequested);
    QSignalSpy toggled(&list, &ActionList::enabledToggled);
    list.card(QStringLiteral("home"))->editLink()->click();
    list.card(QStringLiteral("log-it"))->deleteLink()->click();
    list.card(QStringLiteral("home"))->switchButton()->toggle();
    REQUIRE(edit.count() == 1);
    CHECK(edit.at(0).at(0).toString() == QStringLiteral("home"));
    REQUIRE(remove.count() == 1);
    CHECK(remove.at(0).at(0).toString() == QStringLiteral("log-it"));
    REQUIRE(toggled.count() == 1);
    CHECK(toggled.at(0).at(1).toBool());
    CHECK(list.card(QStringLiteral("nothing")) == nullptr);
}
