// Dictionary words: Tab follows the list as drawn, and focus survives Remove
// (tests/test_personalize_fix2_g2_dictionary_focus.py), in a shown, active Settings window as
// OpenWhisper's Enter-key tests make it. OpenWhisper's "local_whisper" engine (one whose
// steering switch takes focus) is the API engine here.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <QApplication>
#include <QLineEdit>
#include <QTest>

#include "pages_support.h"
#include "settings/keys.h"
#include "ui_personalize/dictionary_library.h"
#include "ui_personalize/dictionary_page.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/settings_switch.h"

using fv::Json;
using namespace fvtest;

namespace {

const std::string kKey(fv::keys::dictation_dictionary.key());

Json twoWords() {
    return Json::array({word("a", "Ksenia", true), word("b", "Oluwaseun")});
}

Json withWords(Json words, Json values = Json::object()) {
    values[kKey] = std::move(words);
    return values;
}

void focus(QWidget* widget) {
    widget->setFocus(Qt::TabFocusReason);
    flushDeletes();
}

void press(Qt::Key key, Qt::KeyboardModifiers modifier = Qt::NoModifier) {
    QTest::keyClick(QApplication::focusWidget(), key, modifier);
    flushDeletes();
}

QString focusedName() {
    QWidget* widget = QApplication::focusWidget();
    return widget != nullptr ? widget->accessibleName() : QString();
}

std::vector<QString> tabWalk(int count) {
    std::vector<QString> names;
    for (int i = 0; i < count; ++i) {
        press(Qt::Key_Tab);
        names.push_back(focusedName());
    }
    return names;
}

// Ten plain words and "Ksenia": enough for the search box.
Json elevenWords() {
    Json words = Json::array();
    for (int index = 0; index < 10; ++index) {
        words.push_back(word(std::to_string(index), "Word" + std::to_string(index)));
    }
    words.push_back(word("k", "Ksenia"));
    return words;
}

struct Opened {
    fv::SettingsDialog& dialog;
    fv::DictionaryPage& page;
};

Opened open(PagesRig& rig) {
    fv::SettingsDialog& dialog = rig.shownDialog();
    fv::DictionaryPage& page = rig.dictionary();
    flushDeletes();
    return {dialog, page};
}

} // namespace

// from
// tests/test_personalize_fix2_g2_dictionary_focus.py::test_tab_reaches_the_words_in_order_before_the_switches_below
TEST_CASE("test_tab_reaches_the_words_in_order_before_the_switches_below",
          "[test_personalize_fix2_g2_dictionary_focus][widget]") {
    // An engine that takes hints, so its steering switch can take focus.
    PagesRig rig(withWords(twoWords(), Json{{"selected_model", "api"}}));
    auto [dialog, page] = open(rig);
    focus(page.addButton());
    CHECK(tabWalk(7) ==
          std::vector<QString>{QStringLiteral("Unstar Ksenia"), QStringLiteral("Edit Ksenia"),
                               QStringLiteral("Remove Ksenia"), QStringLiteral("Star Oluwaseun"),
                               QStringLiteral("Edit Oluwaseun"), QStringLiteral("Remove Oluwaseun"),
                               page.steerSwitch()->accessibleName()});
    focus(page.library()->row(QStringLiteral("a"))->star());
    press(Qt::Key_Backtab, Qt::ShiftModifier);
    CHECK(QApplication::focusWidget() == page.addButton());
    dialog.hide();
}

// from tests/test_personalize_fix2_g2_dictionary_focus.py::test_a_new_word_is_reached_first
TEST_CASE("test_a_new_word_is_reached_first",
          "[test_personalize_fix2_g2_dictionary_focus][widget]") {
    PagesRig rig(withWords(twoWords()));
    auto [dialog, page] = open(rig);
    page.termEdit()->setText(QStringLiteral("Zed"));
    page.addButton()->click();
    flushDeletes();
    CHECK(page.library()->visibleIds().front() != QStringLiteral("a"));
    focus(page.addButton());
    CHECK(tabWalk(4) == std::vector<QString>{QStringLiteral("Star Zed"), QStringLiteral("Edit Zed"),
                                             QStringLiteral("Remove Zed"),
                                             QStringLiteral("Unstar Ksenia")});
    dialog.hide();
}

// from
// tests/test_personalize_fix2_g2_dictionary_focus.py::test_removing_the_bottom_word_focuses_the_one_above_then_the_word_field
TEST_CASE("test_removing_the_bottom_word_focuses_the_one_above_then_the_word_field",
          "[test_personalize_fix2_g2_dictionary_focus][widget]") {
    PagesRig rig(withWords(twoWords()));
    auto [dialog, page] = open(rig);
    focus(page.library()->row(QStringLiteral("b"))->remove());
    press(Qt::Key_Space);
    CHECK(rig.terms() == std::vector<std::string>{"Ksenia"});
    CHECK(focusedName() == QStringLiteral("Remove Ksenia"));
    press(Qt::Key_Space);
    CHECK(rig.terms().empty());
    CHECK(QApplication::focusWidget() == page.termEdit());
    CHECK(dialog.rail()->currentKey() == qs(dest::dictionary));
    dialog.hide();
}

// from
// tests/test_personalize_fix2_g2_dictionary_focus.py::test_removing_a_word_focuses_the_one_that_takes_its_place
TEST_CASE("test_removing_a_word_focuses_the_one_that_takes_its_place",
          "[test_personalize_fix2_g2_dictionary_focus][widget]") {
    Json words = twoWords();
    words.push_back(word("c", "Kubernetes"));
    PagesRig rig(withWords(words));
    auto [dialog, page] = open(rig);
    focus(page.library()->row(QStringLiteral("b"))->remove());
    press(Qt::Key_Space);
    CHECK(rig.terms() == std::vector<std::string>{"Ksenia", "Kubernetes"});
    CHECK(focusedName() == QStringLiteral("Remove Kubernetes"));
    dialog.hide();
}

// from
// tests/test_personalize_fix2_g2_dictionary_focus.py::test_undoing_a_learned_word_moves_focus_like_remove
TEST_CASE("test_undoing_a_learned_word_moves_focus_like_remove",
          "[test_personalize_fix2_g2_dictionary_focus][widget]") {
    Json words = twoWords();
    words.push_back(word("c", "Kubernetes", false, {}, true, true));
    PagesRig rig(withWords(words));
    auto [dialog, page] = open(rig);
    focus(page.library()->row(QStringLiteral("c"))->remove());
    CHECK(focusedName() == QStringLiteral("Undo Kubernetes"));
    press(Qt::Key_Space);
    CHECK(rig.terms() == std::vector<std::string>{"Ksenia", "Oluwaseun"});
    CHECK(focusedName() == QStringLiteral("Remove Oluwaseun"));
    dialog.hide();
}

// from
// tests/test_personalize_fix2_g2_dictionary_focus.py::test_removing_the_only_search_match_focuses_the_search
TEST_CASE("test_removing_the_only_search_match_focuses_the_search",
          "[test_personalize_fix2_g2_dictionary_focus][widget]") {
    PagesRig rig(withWords(elevenWords()));
    auto [dialog, page] = open(rig);
    page.library()->search()->setText(QStringLiteral("Ksen"));
    flushDeletes();
    focus(page.library()->row(QStringLiteral("k"))->remove());
    press(Qt::Key_Space);
    const std::vector<std::string> terms = rig.terms();
    CHECK(std::ranges::find(terms, "Ksenia") == terms.end());
    CHECK(QApplication::focusWidget() == page.library()->search());
    dialog.hide();
}

// from
// tests/test_personalize_fix2_g2_dictionary_focus.py::test_starring_by_keyboard_keeps_focus_on_the_star
TEST_CASE("test_starring_by_keyboard_keeps_focus_on_the_star",
          "[test_personalize_fix2_g2_dictionary_focus][widget]") {
    PagesRig rig(withWords(twoWords()));
    auto [dialog, page] = open(rig);
    focus(page.library()->row(QStringLiteral("b"))->star());
    press(Qt::Key_Space);
    std::vector<bool> starred;
    for (const auto& term : rig.words()) {
        starred.push_back(term.starred);
    }
    CHECK(starred == std::vector<bool>{true, true});
    CHECK(focusedName() == QStringLiteral("Unstar Oluwaseun"));
    dialog.hide();
}

// from
// tests/test_personalize_fix2_g2_dictionary_focus.py::test_typing_a_search_leaves_focus_in_the_search
TEST_CASE("test_typing_a_search_leaves_focus_in_the_search",
          "[test_personalize_fix2_g2_dictionary_focus][widget]") {
    const QString text = GENERATE(QString(), QStringLiteral("Kse"));
    PagesRig rig(withWords(elevenWords()));
    auto [dialog, page] = open(rig);
    page.library()->search()->setText(text);
    focus(page.library()->search());
    QTest::keyClicks(page.library()->search(), QStringLiteral("n"));
    flushDeletes();
    CHECK(QApplication::focusWidget() == page.library()->search());
    dialog.hide();
}
