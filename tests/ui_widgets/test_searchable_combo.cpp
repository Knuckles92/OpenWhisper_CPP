// The type-to-filter dropdown of SearchableComboBox.
// From tests/test_searchable_combo.py.
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <QAbstractItemView>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QSignalSpy>
#include <QTest>

#include "ui_widgets/searchable_combo.h"

namespace {

const QStringList kModels{
    QStringLiteral("openai/gpt-4o"),
    QStringLiteral("openai/gpt-4o-mini"),
    QStringLiteral("openai/o4-mini"),
    QStringLiteral("anthropic/claude-3.5-sonnet"),
    QStringLiteral("anthropic/claude-3-haiku"),
    QStringLiteral("google/gemini-pro-1.5"),
    QStringLiteral("meta-llama/llama-3.1-70b"),
};
const QStringList kClaudeModels{
    QStringLiteral("anthropic/claude-3.5-sonnet"),
    QStringLiteral("anthropic/claude-3-haiku"),
};

// The item texts of the rows not hidden in the combo's dropdown view.
QStringList visibleRows(fv::SearchableComboBox& combo) {
    auto* view = qobject_cast<QListView*>(combo.view());
    REQUIRE(view != nullptr);
    QStringList rows;
    for (int row = 0; row < combo.model()->rowCount(); ++row) {
        if (!view->isRowHidden(row)) {
            rows << combo.model()->data(combo.model()->index(row, 0)).toString();
        }
    }
    return rows;
}

bool popupVisible(fv::SearchableComboBox& combo) {
    return combo.view()->window()->isVisible();
}

// The fixture of TestSearchableComboBox: the models listed, the first current, shown.
struct Fixture {
    Fixture() {
        combo = std::make_unique<fv::SearchableComboBox>();
        combo->addItems(kModels);
        combo->setCurrentText(QStringLiteral("openai/gpt-4o"));
        combo->show();
        combo->activateWindow();
        QApplication::processEvents();
    }
    ~Fixture() {
        combo->hidePopup();
        combo->close();
        QApplication::processEvents();
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    // Types into the line edit, where the real search path keeps focus.
    void typeSearch(const QString& text) {
        QLineEdit* editor = combo->lineEdit();
        editor->setFocus();
        QApplication::processEvents();
        QTest::keyClicks(editor, text);
        QTest::qWait(50);
        QApplication::processEvents();
    }

    std::unique_ptr<fv::SearchableComboBox> combo;
};

} // namespace

// from
// tests/test_searchable_combo.py::TestSearchableComboBox::test_typing_with_dropdown_open_filters_items
TEST_CASE("typing with dropdown open filters items", "[test_searchable_combo][widget]") {
    Fixture f;
    f.combo->showPopup();
    f.typeSearch(QStringLiteral("claude"));
    CHECK(f.combo->currentText() == QStringLiteral("claude"));
    CHECK(visibleRows(*f.combo) == kClaudeModels);
    CHECK(popupVisible(*f.combo));
    CHECK(f.combo->lineEdit()->hasFocus());
}

// from
// tests/test_searchable_combo.py::TestSearchableComboBox::test_typing_replaces_instead_of_appending
TEST_CASE("typing replaces instead of appending", "[test_searchable_combo][widget]") {
    Fixture f;
    f.combo->showPopup();
    f.typeSearch(QStringLiteral("llama"));
    CHECK(f.combo->currentText() == QStringLiteral("llama"));
    CHECK(visibleRows(*f.combo) == QStringList{QStringLiteral("meta-llama/llama-3.1-70b")});
    CHECK(f.combo->lineEdit()->hasFocus());
}

// from
// tests/test_searchable_combo.py::TestSearchableComboBox::test_focus_stays_on_line_edit_while_typing
TEST_CASE("focus stays on line edit while typing", "[test_searchable_combo][widget]") {
    Fixture f;
    f.combo->showPopup();
    QLineEdit* editor = f.combo->lineEdit();
    editor->setFocus();
    for (const QChar ch : QStringLiteral("claude")) {
        QTest::keyClick(editor, ch.toLatin1());
        QApplication::processEvents();
        INFO("after typing " << ch.toLatin1());
        CHECK(editor->hasFocus());
    }
    CHECK(f.combo->currentText() == QStringLiteral("claude"));
    CHECK(visibleRows(*f.combo) == kClaudeModels);
}

// from tests/test_searchable_combo.py::TestSearchableComboBox::test_backspace_widens_filter
TEST_CASE("backspace widens filter", "[test_searchable_combo][widget]") {
    Fixture f;
    f.combo->showPopup();
    f.typeSearch(QStringLiteral("claude-3-h"));
    CHECK(visibleRows(*f.combo) == QStringList{QStringLiteral("anthropic/claude-3-haiku")});
    QLineEdit* editor = f.combo->lineEdit();
    QTest::keyClick(editor, Qt::Key_Backspace);
    QTest::keyClick(editor, Qt::Key_Backspace);
    QTest::qWait(50);
    CHECK(visibleRows(*f.combo) == kClaudeModels);
    CHECK(editor->hasFocus());
}

// from tests/test_searchable_combo.py::TestSearchableComboBox::test_fresh_open_resets_filter
TEST_CASE("fresh open resets filter", "[test_searchable_combo][widget]") {
    Fixture f;
    f.combo->showPopup();
    f.typeSearch(QStringLiteral("claude"));
    f.combo->hidePopup();
    f.combo->showPopup();
    CHECK(visibleRows(*f.combo).size() == kModels.size());
}

// from
// tests/test_searchable_combo.py::TestSearchableComboBox::test_typing_with_dropdown_closed_opens_filtered
TEST_CASE("typing with dropdown closed opens filtered", "[test_searchable_combo][widget]") {
    Fixture f;
    f.combo->lineEdit()->setFocus();
    f.combo->lineEdit()->selectAll();
    QString prefix;
    for (const char ch : {'g', 'p', 't'}) {
        prefix += QLatin1Char(ch);
        QWidget* target = QApplication::focusWidget();
        QTest::keyClick(target != nullptr ? target : f.combo.get(), ch);
        QApplication::processEvents();
        CHECK(f.combo->currentText() == prefix);
    }
    CHECK(popupVisible(*f.combo));
    CHECK(visibleRows(*f.combo) ==
          QStringList{QStringLiteral("openai/gpt-4o"), QStringLiteral("openai/gpt-4o-mini")});
}

// from
// tests/test_searchable_combo.py::TestSearchableComboBox::test_combo_frame_typing_never_activates_an_intermediate_match
TEST_CASE("combo frame typing never activates an intermediate match",
          "[test_searchable_combo][widget]") {
    Fixture f;
    QSignalSpy activated(f.combo.get(), &QComboBox::activated);
    f.combo->showPopup();
    f.combo->setFocus();
    QString prefix;
    for (const char ch : {'c', 'l', 'a', 'u', 'd', 'e'}) {
        prefix += QLatin1Char(ch);
        QTest::keyClick(f.combo.get(), ch);
        QApplication::processEvents();
        CHECK(f.combo->currentText() == prefix);
        CHECK(activated.isEmpty());
        CHECK(popupVisible(*f.combo));
    }
    CHECK(visibleRows(*f.combo) == kClaudeModels);
}

// from
// tests/test_searchable_combo.py::TestSearchableComboBox::test_typing_from_closed_combo_replaces_current_selection
TEST_CASE("typing from closed combo replaces current selection",
          "[test_searchable_combo][widget]") {
    Fixture f;
    f.combo->setFocus();
    QTest::keyClicks(f.combo.get(), QStringLiteral("gpt"));
    QApplication::processEvents();
    CHECK(f.combo->currentText() == QStringLiteral("gpt"));
    CHECK(visibleRows(*f.combo) ==
          QStringList{QStringLiteral("openai/gpt-4o"), QStringLiteral("openai/gpt-4o-mini")});
    CHECK(popupVisible(*f.combo));
}

// from tests/test_searchable_combo.py::TestSearchableComboBox::test_filter_is_case_insensitive
TEST_CASE("filter is case insensitive", "[test_searchable_combo][widget]") {
    Fixture f;
    f.combo->showPopup();
    f.typeSearch(QStringLiteral("CLAUDE"));
    CHECK(visibleRows(*f.combo) == kClaudeModels);
}

// from
// tests/test_searchable_combo.py::TestSearchableComboBox::test_enter_activates_first_visible_match
TEST_CASE("enter activates first visible match", "[test_searchable_combo][widget]") {
    Fixture f;
    QSignalSpy activated(f.combo.get(), &QComboBox::textActivated);
    f.combo->showPopup();
    f.typeSearch(QStringLiteral("claude"));
    QTest::keyClick(f.combo->lineEdit(), Qt::Key_Return);
    QTest::qWait(50);
    CHECK(f.combo->currentText() == QStringLiteral("anthropic/claude-3.5-sonnet"));
    REQUIRE(activated.size() == 1);
    CHECK(activated.at(0).at(0).toString() == QStringLiteral("anthropic/claude-3.5-sonnet"));
    CHECK_FALSE(popupVisible(*f.combo));
}

// from
// tests/test_searchable_combo.py::TestSearchableComboBox::test_badge_reserves_editor_room_and_clears
TEST_CASE("badge reserves editor room and clears", "[test_searchable_combo][widget]") {
    Fixture f;
    f.combo->setBadge(QStringLiteral("Active"));
    QApplication::processEvents();
    const int badgeLeft = f.combo->badge()->geometry().left();
    CHECK(f.combo->badgeText() == QStringLiteral("Active"));
    CHECK(f.combo->lineEdit()->textMargins().right() > 0);
    CHECK(badgeLeft < f.combo->width());

    f.combo->setBadge(QString());
    CHECK(f.combo->badgeText().isEmpty());
    CHECK(f.combo->lineEdit()->textMargins().right() == 0);
}

// from
// tests/test_searchable_combo.py::TestSearchableComboBox::test_repopulate_keeps_plain_combo_semantics
TEST_CASE("repopulate keeps plain combo semantics", "[test_searchable_combo][widget]") {
    Fixture f;
    f.combo->blockSignals(true);
    f.combo->clear();
    f.combo->addItems(kModels);
    f.combo->setCurrentText(QStringLiteral("openai/o4-mini"));
    f.combo->blockSignals(false);
    QApplication::processEvents();
    CHECK(f.combo->currentText() == QStringLiteral("openai/o4-mini"));
    CHECK(visibleRows(*f.combo).size() == kModels.size());
    CHECK_FALSE(popupVisible(*f.combo));
}

// from tests/test_searchable_combo.py::TestSearchableComboBox::test_fuzzy_search_space_vs_hyphen
TEST_CASE("fuzzy search space vs hyphen", "[test_searchable_combo][widget]") {
    Fixture f;
    f.combo->showPopup();
    f.typeSearch(QStringLiteral("gemini 1.5"));
    CHECK(visibleRows(*f.combo) == QStringList{QStringLiteral("google/gemini-pro-1.5")});
    f.combo->hidePopup();

    f.combo->showPopup();
    f.typeSearch(QStringLiteral("gpt 4o"));
    CHECK(visibleRows(*f.combo) ==
          QStringList{QStringLiteral("openai/gpt-4o"), QStringLiteral("openai/gpt-4o-mini")});
}

// from
// tests/test_searchable_combo.py::TestSearchableComboBox::test_fuzzy_search_multi_token_out_of_order
TEST_CASE("fuzzy search multi token out of order", "[test_searchable_combo][widget]") {
    Fixture f;
    f.combo->showPopup();
    f.typeSearch(QStringLiteral("sonnet claude"));
    CHECK(visibleRows(*f.combo) == QStringList{QStringLiteral("anthropic/claude-3.5-sonnet")});
}

// from
// tests/test_searchable_combo.py::TestSearchableComboBox::test_fuzzy_search_best_match_activated_on_enter
TEST_CASE("fuzzy search best match activated on enter", "[test_searchable_combo][widget]") {
    Fixture f;
    QSignalSpy activated(f.combo.get(), &QComboBox::textActivated);
    QSignalSpy activatedRows(f.combo.get(), &QComboBox::activated);
    f.combo->showPopup();
    f.typeSearch(QStringLiteral("gpt 4o"));
    QTest::keyClick(f.combo->lineEdit(), Qt::Key_Return);
    QTest::qWait(50);
    CHECK(f.combo->currentText() == QStringLiteral("openai/gpt-4o"));
    REQUIRE(activated.size() == 1);
    CHECK(activated.at(0).at(0).toString() == QStringLiteral("openai/gpt-4o"));
    // The row of the pick, emitted once whether or not the platform emitted it.
    REQUIRE(activatedRows.size() == 1);
    CHECK(activatedRows.at(0).at(0).toInt() == 0);
    CHECK_FALSE(popupVisible(*f.combo));
}

TEST_CASE("up and down wrap over the visible rows, escape closes", "[widget]") {
    Fixture f;
    f.combo->showPopup();
    f.typeSearch(QStringLiteral("claude"));
    QAbstractItemView* view = f.combo->view();
    QLineEdit* editor = f.combo->lineEdit();
    CHECK(view->currentIndex().row() == 3);
    QTest::keyClick(editor, Qt::Key_Down);
    CHECK(view->currentIndex().row() == 4);
    QTest::keyClick(editor, Qt::Key_Down);
    CHECK(view->currentIndex().row() == 3);
    QTest::keyClick(editor, Qt::Key_Up);
    CHECK(view->currentIndex().row() == 4);
    // Highlighting never changes the chosen value.
    CHECK(f.combo->currentText() == QStringLiteral("claude"));
    QTest::keyClick(editor, Qt::Key_Escape);
    CHECK_FALSE(popupVisible(*f.combo));
}

TEST_CASE("the badge text is stripped and its label is styled on its own", "[widget]") {
    fv::SearchableComboBox combo;
    combo.setBadge(QStringLiteral("  Active \n"));
    CHECK(combo.badgeText() == QStringLiteral("Active"));
    CHECK(combo.badge()->objectName() == QStringLiteral("searchableComboBadge"));
    CHECK_FALSE(combo.badge()->isHidden());
    CHECK(combo.isEditable());
    CHECK(combo.insertPolicy() == QComboBox::NoInsert);
}
