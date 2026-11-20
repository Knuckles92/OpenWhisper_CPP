// Settings > Personalize > Dictionary: adding, starring, editing, learning, the cleanup gate,
// the Home row and the layout, inside a real Settings window over an in-memory settings file.
// OpenWhisper's "local_whisper" engine (an engine that takes hints) is the API engine here:
// OpenWhisper_CPP has no local Whisper.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <QAbstractButton>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>

#include "pages_support.h"
#include "settings/keys.h"
#include "text/dictionary.h"
#include "ui_personalize/cleanup_gates.h"
#include "ui_personalize/dictionary_library.h"
#include "ui_personalize/dictionary_page.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/empty_state.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using namespace fvtest;
namespace keys = fv::keys;

namespace {

const std::string kKey(keys::dictation_dictionary.key());
const std::string kCleanup(keys::transcript_cleanup_enabled.key());

// test_personalize_s4_settings_page.py's WORDS.
Json fourWords() {
    return Json::array({
        word("a", "Ksenia", true, {"Sonia"}),
        word("b", "Oluwaseun", false, {}, true, true),
        word("c", "Kubernetes", false, {"cooper netties"}),
        word("d", "Siobhan Ó Floinn-MacNamara of the Very Long Surname Company", false,
             {"shivon", "shavaun"}),
    });
}

const QString kAddedHint =
    QStringLiteral("Added “Ksenia”. Without a Sounds like spelling it won't be fixed while AI "
                   "cleanup is off.");

QString add(fv::DictionaryPage& page, const QString& term, const QString& heard = {}) {
    page.termEdit()->setText(term);
    page.heardEdit()->setText(heard);
    page.addButton()->click();
    return page.message()->text();
}

fv::Button* gateButton(PagesRig& rig, fv::DictionaryPage& page) {
    fv::Button* button = rig.gates().turnOnButton(page.cleanupGate());
    REQUIRE(button != nullptr);
    return button;
}

std::vector<QString> ids(std::initializer_list<const char*> names) {
    std::vector<QString> out;
    for (const char* name : names) {
        out.push_back(QString::fromUtf8(name));
    }
    return out;
}

} // namespace

// --- tests/test_personalize_s4_settings_page.py ---

// from
// tests/test_personalize_s4_settings_page.py::TestComposer::test_enter_adds_a_word_with_its_variants
TEST_CASE("test_enter_adds_a_word_with_its_variants",
          "[test_personalize_s4_settings_page][widget]") {
    PagesRig rig;
    fv::DictionaryPage& page = rig.dictionary();
    CHECK(rig.railValue(dest::dictionary) == QStringLiteral("No words yet"));
    page.termEdit()->setText(QStringLiteral("  Ksenia "));
    page.heardEdit()->setText(QStringLiteral("Sonia, Senya"));
    QTest::keyClick(page.heardEdit(), Qt::Key_Return);
    const auto words = rig.words();
    REQUIRE(words.size() == 1);
    CHECK(words[0].term == "Ksenia");
    CHECK(words[0].heard == std::vector<std::string>{"Sonia", "Senya"});
    CHECK(page.message()->text() == QStringLiteral("Added “Ksenia”."));
    CHECK(page.message()->property("tone").toString() == QStringLiteral("ok"));
    CHECK(page.termEdit()->text().isEmpty());
    CHECK(page.heardEdit()->text().isEmpty());
    CHECK(page.library()->visibleIds() == std::vector<QString>{qs(words[0].id)});
    CHECK(rig.railValue(dest::dictionary) == QStringLiteral("1 word"));
    REQUIRE_FALSE(rig.changes.isEmpty());
    CHECK(rig.changes.back() == QStringLiteral("dictionary"));
}

// from
// tests/test_personalize_s4_settings_page.py::TestComposer::test_problems_are_explained_and_nothing_changes
TEST_CASE("test_problems_are_explained_and_nothing_changes",
          "[test_personalize_s4_settings_page][widget]") {
    const auto [term, expected] =
        GENERATE(std::pair<const char*, const char*>{"", "Type a word"},
                 std::pair<const char*, const char*>{"Ksenia", "already"});
    PagesRig rig(Json{{kKey, Json::array({fourWords()[0]})}});
    fv::DictionaryPage& page = rig.dictionary();
    page.termEdit()->setText(QString::fromUtf8(term));
    page.addButton()->click();
    CHECK(page.message()->text().contains(QString::fromUtf8(expected)));
    CHECK(page.message()->property("tone").toString() == QStringLiteral("error"));
    CHECK(rig.terms() == std::vector<std::string>{"Ksenia"});
    page.termEdit()->setText(QStringLiteral("Ksenia2"));
    QTest::keyClick(page.termEdit(), Qt::Key_A);
    CHECK(page.message()->isHidden());
}

// from tests/test_personalize_s4_settings_page.py::TestComposer::test_a_failed_save_says_so
TEST_CASE("test_a_failed_save_says_so", "[test_personalize_s4_settings_page][widget]") {
    PagesRig rig;
    fv::DictionaryPage& page = rig.dictionary();
    rig.settings().file().fail_next_replace("disk full");
    page.termEdit()->setText(QStringLiteral("Ksenia"));
    page.addButton()->click();
    CHECK(page.message()->text() == QStringLiteral("Couldn't add the word. Try again."));
    CHECK(rig.words().empty());
}

// from tests/test_personalize_s4_settings_page.py::TestComposer::test_editing_a_word
TEST_CASE("test_editing_a_word", "[test_personalize_s4_settings_page][widget]") {
    PagesRig rig(Json{{kKey, fourWords()}});
    fv::DictionaryPage& page = rig.dictionary();
    page.library()->row(QStringLiteral("c"))->edit()->click();
    CHECK(page.composer()->titleLabel()->text() == QStringLiteral("Edit a word"));
    CHECK(page.termEdit()->text() == QStringLiteral("Kubernetes"));
    CHECK(page.heardEdit()->text() == QStringLiteral("cooper netties"));
    CHECK(page.addButton()->text() == QStringLiteral("Save"));
    CHECK_FALSE(page.cancelButton()->isHidden());
    page.termEdit()->setText(QStringLiteral("K8s"));
    page.heardEdit()->setText(QStringLiteral("kates"));
    page.addButton()->click();
    const auto words = rig.words();
    const auto edited = std::ranges::find_if(words, [](const auto& w) { return w.id == "c"; });
    REQUIRE(edited != words.end());
    CHECK(edited->term == "K8s");
    CHECK(edited->heard == std::vector<std::string>{"kates"});
    CHECK(page.message()->text() == QStringLiteral("Saved “K8s”."));
    CHECK(page.addButton()->text() == QStringLiteral("Add"));
    CHECK(page.cancelButton()->isHidden());
}

// from tests/test_personalize_s4_settings_page.py::TestComposer::test_cancelling_an_edit
TEST_CASE("test_cancelling_an_edit", "[test_personalize_s4_settings_page][widget]") {
    PagesRig rig(Json{{kKey, fourWords()}});
    fv::DictionaryPage& page = rig.dictionary();
    page.library()->row(QStringLiteral("a"))->edit()->click();
    page.cancelButton()->click();
    CHECK(page.termEdit()->text().isEmpty());
    CHECK(page.composer()->titleLabel()->text() == QStringLiteral("Add a word"));
    page.termEdit()->setText(QStringLiteral("Olu"));
    page.addButton()->click();
    CHECK(rig.terms().front() == "Olu");
}

// from
// tests/test_personalize_s4_settings_page.py::TestLibrary::test_rows_show_stars_badges_and_variants
TEST_CASE("test_rows_show_stars_badges_and_variants",
          "[test_personalize_s4_settings_page][widget]") {
    PagesRig rig(Json{{kKey, fourWords()}});
    fv::DictionaryPage& page = rig.dictionary();
    CHECK(page.library()->visibleIds() == ids({"a", "b", "c", "d"}));
    fv::DictionaryRow* ksenia = page.library()->row(QStringLiteral("a"));
    fv::DictionaryRow* olu = page.library()->row(QStringLiteral("b"));
    REQUIRE(ksenia != nullptr);
    REQUIRE(olu != nullptr);
    CHECK(ksenia->star()->isChecked());
    CHECK(ksenia->star()->property("starred").toBool());
    CHECK(ksenia->detail()->text() == QStringLiteral("Sounds like Sonia"));
    CHECK(ksenia->learnedBadge()->isHidden());
    CHECK(ksenia->remove()->text() == QStringLiteral("Remove"));
    CHECK_FALSE(olu->learnedBadge()->isHidden());
    CHECK_FALSE(olu->newBadge()->isHidden());
    CHECK(olu->remove()->text() == QStringLiteral("Undo"));
    CHECK(rig.railValue(dest::dictionary) == QStringLiteral("4 words · 1 new"));
    CHECK_FALSE(page.newTile()->isHidden());
    CHECK(page.newTile()->descriptionLabel()->text().contains(QStringLiteral("1 new word")));
}

// from tests/test_personalize_s4_settings_page.py::TestLibrary::test_star_remove_and_undo
TEST_CASE("test_star_remove_and_undo", "[test_personalize_s4_settings_page][widget]") {
    PagesRig rig(Json{{kKey, fourWords()}});
    fv::DictionaryPage& page = rig.dictionary();
    page.library()->row(QStringLiteral("c"))->star()->click();
    const auto words = rig.words();
    CHECK(std::ranges::find_if(words, [](const auto& w) { return w.id == "c"; })->starred);
    CHECK(page.library()->row(QStringLiteral("c"))->star()->property("starred").toBool());
    page.library()->row(QStringLiteral("b"))->remove()->click();
    CHECK(page.message()->text() == QStringLiteral("Forgot “Oluwaseun”."));
    page.library()->row(QStringLiteral("d"))->remove()->click();
    std::vector<std::string> left;
    for (const auto& w : rig.words()) {
        left.push_back(w.id);
    }
    CHECK(left == std::vector<std::string>{"a", "c"});
    CHECK(page.library()->visibleIds() == ids({"a", "c"}));
    CHECK(page.newTile()->isHidden());
}

// from tests/test_personalize_s4_settings_page.py::TestLibrary::test_keep_all_clears_the_new_badges
TEST_CASE("test_keep_all_clears_the_new_badges", "[test_personalize_s4_settings_page][widget]") {
    PagesRig rig(Json{{kKey, fourWords()}});
    fv::DictionaryPage& page = rig.dictionary();
    auto* keep = rig.dialog().findChild<QPushButton*>(QStringLiteral("dictionaryKeepNewButton"));
    REQUIRE(keep != nullptr);
    keep->click();
    const auto words = rig.words();
    CHECK(std::ranges::none_of(words, [](const auto& w) { return w.is_new; }));
    CHECK(words[1].learned);
    CHECK(page.newTile()->isHidden());
    CHECK(page.library()->row(QStringLiteral("b"))->remove()->text() == QStringLiteral("Remove"));
    CHECK(rig.railValue(dest::dictionary) == QStringLiteral("4 words"));
}

// from
// tests/test_personalize_s4_settings_page.py::TestLibrary::test_a_long_dictionary_is_capped_and_searchable
TEST_CASE("test_a_long_dictionary_is_capped_and_searchable",
          "[test_personalize_s4_settings_page][widget]") {
    Json words = Json::array();
    for (int i = 0; i < 130; ++i) {
        const std::string number = std::to_string(i);
        words.push_back(Json{{"id", "w" + number},
                             {"term", "Word" + std::string(3 - number.size(), '0') + number}});
    }
    PagesRig rig(Json{{kKey, words}});
    fv::DictionaryLibrary& library = *rig.dictionary().library();
    CHECK(library.visibleIds().size() == fv::DictionaryLibrary::kMaxRows);
    CHECK(library.more()->text() == QStringLiteral("Showing 100 of 130. Search to find the rest."));
    CHECK_FALSE(library.search()->isHidden());
    library.search()->setText(QStringLiteral("word12"));
    std::vector<QString> expected;
    for (int i = 120; i < 130; ++i) {
        expected.push_back(QStringLiteral("w%1").arg(i));
    }
    CHECK(library.visibleIds() == expected);
    CHECK(library.more()->isHidden());
    library.search()->setText(QStringLiteral("zzz"));
    CHECK(library.visibleIds().empty());
    CHECK(library.empty()->text().contains(QStringLiteral("No words match")));
}

// from tests/test_personalize_s4_settings_page.py::TestLibrary::test_empty_dictionary
TEST_CASE("test_empty_dictionary", "[test_personalize_s4_settings_page][widget]") {
    PagesRig rig;
    fv::DictionaryPage& page = rig.dictionary();
    CHECK(page.library()->search()->isHidden());
    CHECK_FALSE(page.library()->empty()->isHidden());
    CHECK(page.library()->empty()->text().contains(QStringLiteral("No words yet")));
    CHECK(page.count()->isHidden());
}

// from tests/test_personalize_s4_settings_page.py::TestRecognitionAndLearning::test_engine_line
// OpenWhisper_CPP's engines; OpenWhisper's local_whisper row has no engine here.
TEST_CASE("test_engine_line", "[test_personalize_s4_settings_page][unit]") {
    const auto line = [](const char* engine) {
        const Json settings{{"selected_model", engine}};
        return fv::text::dictionary_page::engine_line(settings,
                                                      fv::DictionaryPage::engineInfo(settings));
    };
    CHECK(line("parakeet") == "Your engine (Parakeet) can't be steered, and AI cleanup is off, so "
                              "only Sounds like spellings are fixed.");
    CHECK(line("api") == "Your engine (OpenAI) also steers the speech model toward your words. "
                         "They're sent to OpenAI with your audio.");
    CHECK(line("remote") == "Engines that take hints also listen for your words. Every engine "
                            "fixes spellings after recognition.");
}

// from
// tests/test_personalize_s4_settings_page.py::TestRecognitionAndLearning::test_engine_line_names_the_api_server
TEST_CASE("test_engine_line_names_the_api_server", "[test_personalize_s4_settings_page][unit]") {
    const auto line = [](const char* server) {
        const Json settings{{"selected_model", "api"}, {"api_transcription_endpoint", server}};
        return fv::text::dictionary_page::engine_line(settings,
                                                      fv::DictionaryPage::engineInfo(settings));
    };
    CHECK(line("speaches") ==
          "Your engine (Speaches) also steers the speech model toward your words.");
    CHECK(line("groq") == "Your engine (Groq) also steers the speech model toward your words. "
                          "They're sent to Groq with your audio.");
}

// from
// tests/test_personalize_s4_settings_page.py::TestRecognitionAndLearning::test_the_page_reads_the_live_backend
TEST_CASE("test_the_page_reads_the_live_backend", "[test_personalize_s4_settings_page][widget]") {
    fv::DictionaryPage::setBackendProvider(
        [] { return fv::BackendInfo{true, true, "model", "Whisper on devbox", "devbox"}; });
    PagesRig rig(Json{{"selected_model", "remote"}});
    fv::DictionaryPage& page = rig.dictionary();
    CHECK(page.steerTile()->descriptionLabel()->text().contains(QStringLiteral("sent to devbox")));
}

// from tests/test_personalize_s4_settings_page.py::TestRecognitionAndLearning::test_steering_switch
TEST_CASE("test_steering_switch", "[test_personalize_s4_settings_page][widget]") {
    // An engine that takes hints; cleanup on, so the Off line still names it.
    PagesRig rig(Json{{"selected_model", "api"}, {kCleanup, true}});
    fv::DictionaryPage& page = rig.dictionary();
    CHECK(page.steerSwitch()->isChecked());
    page.steerSwitch()->click();
    CHECK(rig.saved(keys::dictionary_steer_recognition.key()) == false);
    CHECK(page.steerTile()->descriptionLabel()->text() ==
          QStringLiteral(
              "Off. Your dictionary still fixes spellings after recognition and in AI cleanup."));
    CHECK(page.steerTile()->property("checked") == QVariant(false));
}

// from
// tests/test_personalize_s4_settings_page.py::TestRecognitionAndLearning::test_learning_waits_for_text_reading
TEST_CASE("test_learning_waits_for_text_reading", "[test_personalize_s4_settings_page][widget]") {
    PagesRig rig;
    fv::DictionaryPage& page = rig.dictionary();
    CHECK(page.learnTile()->isHidden());
    CHECK_FALSE(page.learnGateTile()->isHidden());
    CHECK(page.learnGateTile()->descriptionLabel()->text() ==
          QStringLiteral("Off · needs Read text near the cursor"));
    rig.dialog().findChild<QPushButton*>(QStringLiteral("dictionaryLearnTurnOnButton"))->click();
    CHECK(rig.dialog().rail()->currentKey() == qs(dest::styles));
    rig.settings().update(Json{{keys::app_context_read_text.key(), true}});
    rig.dialog().show();
    rig.dialog().selectDestination(qs(dest::dictionary));
    QCoreApplication::processEvents();
    CHECK_FALSE(page.learnTile()->isHidden());
    CHECK(page.learnGateTile()->isHidden());
    page.learnSwitch()->setChecked(false);
    CHECK(rig.saved(keys::dictionary_learn_enabled.key()) == false);
    rig.dialog().close();
}

// from
// tests/test_personalize_s4_settings_page.py::TestRecognitionAndLearning::test_loading_never_writes
TEST_CASE("test_loading_never_writes", "[test_personalize_s4_settings_page][widget]") {
    PagesRig rig(Json{{keys::dictionary_steer_recognition.key(), false}});
    fv::DictionaryPage& page = rig.dictionary();
    const Json before = rig.settings().contents();
    const auto writes = rig.settings().file().writes();
    page.loadSettings(before);
    rig.dialog().refreshPage(dest::dictionary);
    CHECK(rig.settings().contents() == before);
    CHECK(rig.settings().file().writes() == writes);
    CHECK_FALSE(page.steerSwitch()->isChecked());
}

// from
// tests/test_personalize_s4_settings_page.py::TestBasicRow::test_basic_shows_the_count_and_opens_the_composer
TEST_CASE("test_basic_shows_the_count_and_opens_the_composer",
          "[test_personalize_s4_settings_page][widget]") {
    PagesRig rig(Json{{kKey, fourWords()}});
    fv::SettingsDialog& dialog = rig.shownDialog();
    QCoreApplication::processEvents();
    auto* button = rig.home().findChild<QPushButton*>(QStringLiteral("homeDictionaryAddButton"));
    REQUIRE(button != nullptr);
    CHECK(button->text() == QStringLiteral("Add word"));
    CHECK(rig.homeDetails().contains(QStringLiteral(
        "4 words · 1 new · Only Sounds like spellings are fixed while AI cleanup is off.")));
    CHECK_FALSE(dialog.isBuilt(dest::dictionary));
    button->click();
    for (int i = 0; i < 5; ++i) {
        QCoreApplication::processEvents();
    }
    CHECK(dialog.rail()->currentKey() == qs(dest::dictionary));
    auto* term = qobject_cast<QLineEdit*>(dialog.control("dictionary_term_edit"));
    REQUIRE(term != nullptr);
    CHECK(pumpUntil([term] { return term->hasFocus(); }));
    dialog.close();
}

// from tests/test_personalize_s4_settings_page.py::TestBasicRow::test_basic_row_without_words
TEST_CASE("test_basic_row_without_words", "[test_personalize_s4_settings_page][widget]") {
    PagesRig rig(Json{{"selected_model", "api"}});
    CHECK(rig.homeDetails().contains(
        QStringLiteral("Names and terms dictation should always get right.")));
}

// The checks of the narrow-window tests: every visible button and field inside the page, every
// visible tile's labels tall enough for their text, and the page's end reachable by scrolling.
void checkFitsPage(fv::SettingsDialog& dialog, std::string_view key) {
    fv::SettingsPage* page = dialog.page(key);
    REQUIRE(page != nullptr);
    QList<QWidget*> controls;
    for (auto* button : page->findChildren<QAbstractButton*>()) {
        controls << button;
    }
    for (auto* field : page->findChildren<QLineEdit*>()) {
        controls << field;
    }
    for (QWidget* control : controls) {
        if (!control->isVisible()) {
            continue;
        }
        INFO(control->objectName().toStdString());
        CHECK(control->mapTo(page, control->rect().topLeft()).x() >= 0);
        CHECK(control->mapTo(page, control->rect().bottomRight()).x() < page->width());
    }
    for (auto* tile : page->findChildren<fv::TileBase*>()) {
        if (tile->isHidden()) {
            continue;
        }
        for (QLabel* label : {static_cast<QLabel*>(tile->titleLabel()),
                              static_cast<QLabel*>(tile->descriptionLabel())}) {
            CHECK(label->height() >= label->heightForWidth(label->width()));
        }
        CHECK(page->rect().contains(tile->geometry()));
    }
    QScrollArea* scroll = dialog.pageScroll(key);
    REQUIRE(scroll != nullptr);
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    QCoreApplication::processEvents();
    CHECK(page->mapTo(scroll->viewport(), page->rect().bottomRight()).y() <=
          scroll->viewport()->height());
}

// from tests/test_personalize_s4_settings_page.py::test_page_fits_narrow_windows_at_large_fonts
// The classic variant; the omarchy one is Linux's Omarchy desktop look, not ported.
TEST_CASE("test_page_fits_narrow_windows_at_large_fonts",
          "[test_personalize_s4_settings_page][widget]") {
    AppTheme theme(QStringLiteral("dark"), 130);
    PagesRig rig(Json{{kKey, fourWords()},
                      {keys::transcript_cleanup_rules.key(),
                       Json::array({"Always spell my name \"Alex Rivera\""})}});
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    dialog.resize(940, 600);
    fv::DictionaryPage& page = rig.dictionary();
    page.library()->row(QStringLiteral("a"))->edit()->click();
    for (int i = 0; i < 10; ++i) {
        QCoreApplication::processEvents();
    }
    CHECK(dialog.width() == 940);
    checkFitsPage(dialog, dest::dictionary);
    for (const char* id : {"a", "b", "c", "d"}) {
        CHECK_FALSE(page.library()->row(QString::fromUtf8(id))->layoutRow()->isStacked());
    }
    dialog.close();
}

// --- tests/test_personalize_fix3_h2_dictionary_honesty.py: Parakeet with AI cleanup off ---

// from
// tests/test_personalize_fix3_h2_dictionary_honesty.py::TestDefaultWindowsSetup::test_the_page_says_what_happens_and_offers_cleanup
TEST_CASE("test_the_page_says_what_happens_and_offers_cleanup",
          "[test_personalize_fix3_h2_dictionary_honesty][widget]") {
    PagesRig rig;
    fv::DictionaryPage& page = rig.dictionary();
    fv::InfoTile* gate = page.cleanupGate();
    CHECK_FALSE(gate->isHidden());
    CHECK(gate->titleLabel()->text() == QStringLiteral("AI cleanup is off"));
    CHECK(gate->descriptionLabel()->text() ==
          QStringLiteral("Parakeet can't listen for your words, so only Sounds like spellings "
                         "get fixed. AI cleanup uses your words too."));
    CHECK(gateButton(rig, page)->text() == QStringLiteral("Turn on (Medium)"));
}

// from
// tests/test_personalize_fix3_h2_dictionary_honesty.py::TestDefaultWindowsSetup::test_the_steer_switch_says_why_it_does_nothing
TEST_CASE("test_the_steer_switch_says_why_it_does_nothing",
          "[test_personalize_fix3_h2_dictionary_honesty][widget]") {
    PagesRig rig;
    fv::DictionaryPage& page = rig.dictionary();
    CHECK_FALSE(page.steerTile()->isEnabled());
    CHECK(page.steerTile()->descriptionLabel()->text() ==
          QStringLiteral("Your engine (Parakeet) can't be steered, and AI cleanup is off, so only "
                         "Sounds like spellings are fixed."));
}

// from
// tests/test_personalize_fix3_h2_dictionary_honesty.py::TestDefaultWindowsSetup::test_adding_a_word_without_sounds_like_says_it_wont_be_fixed
TEST_CASE("test_adding_a_word_without_sounds_like_says_it_wont_be_fixed",
          "[test_personalize_fix3_h2_dictionary_honesty][widget]") {
    PagesRig rig;
    fv::DictionaryPage& page = rig.dictionary();
    CHECK(add(page, QStringLiteral("Ksenia")) == kAddedHint);
    CHECK(add(page, QStringLiteral("Oluwaseun"), QStringLiteral("Olu Washington")) ==
          QStringLiteral("Added “Oluwaseun”."));
    page.beginEdit(page.library()->visibleIds().front());
    page.heardEdit()->clear();
    page.addButton()->click();
    CHECK(page.message()->text().startsWith(QStringLiteral("Saved “")));
    CHECK(page.message()->text().contains(QStringLiteral("Without a Sounds like spelling")));
}

// from
// tests/test_personalize_fix3_h2_dictionary_honesty.py::TestDefaultWindowsSetup::test_one_click_turns_cleanup_on_and_the_page_updates
TEST_CASE("test_one_click_turns_cleanup_on_and_the_page_updates",
          "[test_personalize_fix3_h2_dictionary_honesty][widget]") {
    PagesRig rig;
    fv::DictionaryPage& page = rig.dictionary();
    gateButton(rig, page)->click();
    CHECK(rig.saved(kCleanup) == true);
    CHECK(page.cleanupGate()->isHidden());
    CHECK(page.steerTile()->descriptionLabel()->text() ==
          QStringLiteral("Your engine (Parakeet) can't be steered. AI cleanup and Sounds like "
                         "spellings fix your words after recognition."));
    CHECK(add(page, QStringLiteral("Ksenia")) == QStringLiteral("Added “Ksenia”."));
    CHECK(rig.changes.contains(QStringLiteral("cleanup")));
}

// from
// tests/test_personalize_fix3_h2_dictionary_honesty.py::TestDefaultWindowsSetup::test_spelling_rules_dont_promise_to_work_without_cleanup
TEST_CASE("test_spelling_rules_dont_promise_to_work_without_cleanup",
          "[test_personalize_fix3_h2_dictionary_honesty][widget]") {
    PagesRig rig(Json{{keys::transcript_cleanup_rules.key(),
                       Json::array({"Always spell my name \"Alex Rivera\""})}});
    fv::DictionaryPage& page = rig.dictionary();
    const QString text = page.rulesTile()->descriptionLabel()->text();
    CHECK_FALSE(text.contains(QStringLiteral("when AI cleanup is off")));
    CHECK(text.startsWith(QStringLiteral("One of your Learned rules only spells a word.")));
}

// from
// tests/test_personalize_fix3_h2_dictionary_honesty.py::TestWhereWordsDoCount::test_no_notice_and_a_plain_confirmation
TEST_CASE("test_no_notice_and_a_plain_confirmation",
          "[test_personalize_fix3_h2_dictionary_honesty][widget]") {
    const Json values =
        GENERATE(Json{{"transcript_cleanup_enabled", true}}, Json{{"selected_model", "api"}});
    PagesRig rig(values);
    fv::DictionaryPage& page = rig.dictionary();
    CHECK(page.cleanupGate()->isHidden());
    CHECK(add(page, QStringLiteral("Ksenia")) == QStringLiteral("Added “Ksenia”."));
}

// from
// tests/test_personalize_fix3_h2_dictionary_honesty.py::TestWhereWordsDoCount::test_engines_that_take_hints_keep_the_switch
TEST_CASE("test_engines_that_take_hints_keep_the_switch",
          "[test_personalize_fix3_h2_dictionary_honesty][widget]") {
    PagesRig rig(Json{{"selected_model", "api"}});
    fv::DictionaryPage& page = rig.dictionary();
    CHECK(page.steerTile()->isEnabled());
    page.steerSwitch()->click();
    CHECK(rig.saved(keys::dictionary_steer_recognition.key()) == false);
    CHECK(page.steerTile()->descriptionLabel()->text() ==
          QStringLiteral("Off. Only Sounds like spellings are fixed while AI cleanup is off."));
    // Steering off: the word reaches neither the model nor cleanup.
    CHECK(add(page, QStringLiteral("Ksenia")) == kAddedHint);
}

// from
// tests/test_personalize_fix3_h2_dictionary_honesty.py::TestWhereWordsDoCount::test_a_spelling_rule_with_a_variant_still_works_without_cleanup
TEST_CASE("test_a_spelling_rule_with_a_variant_still_works_without_cleanup",
          "[test_personalize_fix3_h2_dictionary_honesty][widget]") {
    PagesRig rig(
        Json{{keys::transcript_cleanup_rules.key(), Json::array({"Spell \"jon\" as \"John\"."})}});
    fv::DictionaryPage& page = rig.dictionary();
    CHECK(page.rulesTile()->descriptionLabel()->text().contains(
        QStringLiteral("also works when AI cleanup is off")));
}

// from
// tests/test_personalize_fix3_h2_dictionary_honesty.py::TestWhereWordsDoCount::test_an_unknown_remote_engine_claims_nothing
TEST_CASE("test_an_unknown_remote_engine_claims_nothing",
          "[test_personalize_fix3_h2_dictionary_honesty][widget]") {
    PagesRig rig(Json{{"selected_model", "remote"}});
    fv::DictionaryPage& page = rig.dictionary();
    CHECK(page.cleanupGate()->isHidden());
    CHECK(page.steerTile()->isEnabled());
    CHECK(add(page, QStringLiteral("Ksenia")) == QStringLiteral("Added “Ksenia”."));
}

// from
// tests/test_personalize_fix3_h2_dictionary_honesty.py::TestBasicRow::test_default_setup_says_only_sounds_like_spellings_are_fixed
TEST_CASE("test_default_setup_says_only_sounds_like_spellings_are_fixed",
          "[test_personalize_fix3_h2_dictionary_honesty][widget]") {
    PagesRig rig(Json{{kKey, Json::array({word("a", "Ksenia")})}});
    CHECK(rig.homeDetails().contains(
        QStringLiteral("1 word · Only Sounds like spellings are fixed while AI cleanup is off.")));
}

// from
// tests/test_personalize_fix3_h2_dictionary_honesty.py::TestBasicRow::test_without_words_it_says_what_a_word_needs
TEST_CASE("test_without_words_it_says_what_a_word_needs",
          "[test_personalize_fix3_h2_dictionary_honesty][widget]") {
    PagesRig rig;
    CHECK(rig.homeDetails().contains(QStringLiteral(
        "Names and terms to get right. With AI cleanup off, each needs a Sounds like spelling.")));
}

// from
// tests/test_personalize_fix3_h2_dictionary_honesty.py::TestBasicRow::test_with_cleanup_on_it_just_counts
TEST_CASE("test_with_cleanup_on_it_just_counts",
          "[test_personalize_fix3_h2_dictionary_honesty][widget]") {
    PagesRig rig(Json{{kKey, Json::array({word("a", "Ksenia")})}, {kCleanup, true}});
    CHECK(rig.homeDetails().contains(QStringLiteral("1 word")));
}

// from
// tests/test_personalize_fix3_h2_dictionary_honesty.py::test_notice_fits_narrow_windows_at_large_fonts
// The classic variant; the omarchy one is Linux's Omarchy desktop look, not ported.
TEST_CASE("test_notice_fits_narrow_windows_at_large_fonts",
          "[test_personalize_fix3_h2_dictionary_honesty][widget]") {
    AppTheme theme(QStringLiteral("light"), 130);
    PagesRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    dialog.resize(940, 600);
    fv::DictionaryPage& page = rig.dictionary();
    for (int i = 0; i < 10; ++i) {
        QCoreApplication::processEvents();
    }
    fv::SettingsPage* container = dialog.page(dest::dictionary);
    fv::InfoTile* gate = page.cleanupGate();
    CHECK(gate->isVisible());
    CHECK(container->rect().contains(gate->geometry().translated(page.pos())));
    for (QLabel* label : {static_cast<QLabel*>(gate->titleLabel()),
                          static_cast<QLabel*>(gate->descriptionLabel())}) {
        CHECK(label->height() >= label->heightForWidth(label->width()));
    }
    fv::Button* button = gateButton(rig, page);
    CHECK(button->mapTo(container, button->rect().bottomRight()).x() < container->width());
    dialog.close();
}

// --- The other files' Dictionary tests ---

// from
// tests/test_personalize_fix_f4_styles_gate.py::test_dictionary_learning_gate_says_where_it_goes
TEST_CASE("test_dictionary_learning_gate_says_where_it_goes",
          "[test_personalize_fix_f4_styles_gate][widget]") {
    PagesRig rig;
    rig.dictionary();
    auto* button =
        rig.dialog().findChild<QPushButton*>(QStringLiteral("dictionaryLearnTurnOnButton"));
    REQUIRE(button != nullptr);
    // "&&" draws one "&"; a single one would underline the next letter.
    CHECK(button->text() == QStringLiteral("Apps && styles…"));
    button->click();
    CHECK(rig.dialog().rail()->currentKey() == qs(dest::styles));
}

// from tests/test_personalize_fix2_g2_enter_key.py::test_enter_adds_words_one_after_another
TEST_CASE("test_enter_adds_words_one_after_another",
          "[test_personalize_fix2_g2_enter_key][widget]") {
    const auto [key, modifier] =
        GENERATE(std::pair<Qt::Key, Qt::KeyboardModifier>{Qt::Key_Return, Qt::NoModifier},
                 std::pair<Qt::Key, Qt::KeyboardModifier>{Qt::Key_Enter, Qt::KeypadModifier});
    PagesRig rig;
    fv::SettingsDialog& dialog = rig.shownDialog();
    fv::DictionaryPage& page = rig.dictionary();
    for (const char* term : {"Ksenia", "Oluwaseun"}) {
        page.termEdit()->setFocus(Qt::TabFocusReason);
        QCoreApplication::processEvents();
        QTest::keyClicks(page.termEdit(), QString::fromUtf8(term));
        QTest::keyClick(page.termEdit(), key, modifier);
        QCoreApplication::processEvents();
    }
    std::vector<std::string> terms = rig.terms();
    std::ranges::sort(terms);
    CHECK(terms == std::vector<std::string>{"Ksenia", "Oluwaseun"});
    CHECK(dialog.pageTitleLabel()->text() == QStringLiteral("Dictionary"));
    // Enter never reached a header button: the page menu stayed shut.
    CHECK(dialog.pageMenu() == nullptr);
    CHECK(page.termEdit()->isVisible());
    CHECK(QApplication::focusWidget() == page.termEdit());
    dialog.hide();
}

// from tests/test_personalize_fix2_g2_enter_key.py::test_enter_in_sounds_like_adds_the_word
TEST_CASE("test_enter_in_sounds_like_adds_the_word",
          "[test_personalize_fix2_g2_enter_key][widget]") {
    PagesRig rig;
    fv::SettingsDialog& dialog = rig.shownDialog();
    fv::DictionaryPage& page = rig.dictionary();
    page.termEdit()->setText(QStringLiteral("Siobhan"));
    page.heardEdit()->setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    QTest::keyClicks(page.heardEdit(), QStringLiteral("shivon"));
    QTest::keyClick(page.heardEdit(), Qt::Key_Return);
    QCoreApplication::processEvents();
    const auto words = rig.words();
    REQUIRE(words.size() == 1);
    CHECK(words[0].term == "Siobhan");
    CHECK(words[0].heard == std::vector<std::string>{"shivon"});
    CHECK(dialog.pageTitleLabel()->text() == QStringLiteral("Dictionary"));
    CHECK(dialog.pageMenu() == nullptr);
    dialog.hide();
}

// from tests/test_personalize_fix2_g2_enter_key.py::test_add_word_is_named_after_its_label
TEST_CASE("test_add_word_is_named_after_its_label",
          "[test_personalize_fix2_g2_enter_key][widget]") {
    PagesRig rig;
    auto* button = rig.home().findChild<QPushButton*>(QStringLiteral("homeDictionaryAddButton"));
    REQUIRE(button != nullptr);
    CHECK(button->text() == QStringLiteral("Add word"));
    CHECK(button->accessibleName().startsWith(QStringLiteral("Add word")));
}

// from tests/test_dictionary_cleanup_revert.py::test_a_protected_word_shows_its_badge
TEST_CASE("test_a_protected_word_shows_its_badge", "[test_dictionary_cleanup_revert][widget]") {
    fv::text::DictionaryTerm kept;
    kept.id = "k";
    kept.term = "Ksenia";
    kept.learned = true;
    kept.is_protected = true;
    fv::text::DictionaryTerm plainTerm;
    plainTerm.id = "o";
    plainTerm.term = "Olu";
    fv::DictionaryRow row(kept);
    fv::DictionaryRow plain(plainTerm);
    CHECK_FALSE(row.keptBadge()->isHidden());
    CHECK(plain.keptBadge()->isHidden());
}

// --- What the port adds around the page ---

TEST_CASE("the cleanup gates follow the level and turn cleanup on through the window", "[widget]") {
    PagesRig rig(Json{{"transcript_cleanup_level", "high"}});
    fv::DictionaryPage& page = rig.dictionary();
    CHECK(gateButton(rig, page)->text() == QStringLiteral("Turn on (High)"));
    CHECK(fv::CleanupGates::turnOnLabel(Json{{"transcript_cleanup_prompt", "Bullets only."}}) ==
          QStringLiteral("Turn on (Custom)"));
    CHECK(fv::CleanupGates::turnOnLabel(Json::object()) == QStringLiteral("Turn on (Medium)"));
    CHECK(fv::CleanupGates::turnOnLabel(Json{{"transcript_cleanup_level", "light"}}) ==
          QStringLiteral("Turn on (Light)"));

    // A handler (the AI cleanup page's) takes over the button.
    int handled = 0;
    rig.gates().setTurnOnHandler([&handled] { ++handled; });
    gateButton(rig, page)->click();
    CHECK(handled == 1);
    CHECK_FALSE(rig.settings().snapshot()->contains(kCleanup));
    rig.gates().setTurnOnHandler({});

    // Without a window to save through, the store takes the write.
    rig.gates().turnOnCleanup();
    CHECK(rig.saved(kCleanup) == true);
    CHECK(page.cleanupGate()->isHidden());
}

TEST_CASE("a gate that needs nothing stays hidden, and one that is gone is forgotten", "[widget]") {
    PagesRig rig;
    bool needed = false;
    QWidget holder;
    fv::InfoTile* gate = rig.gates().gateTile(
        QStringLiteral("Words need cleanup."), [&needed] { return needed; }, &holder);
    CHECK(gate->isHidden());
    needed = true;
    rig.gates().refresh(*rig.settings().snapshot());
    CHECK_FALSE(gate->isHidden());
    rig.gates().refresh(Json{{kCleanup, true}});
    CHECK(gate->isHidden());
    delete gate;
    CHECK(rig.gates().turnOnButton(gate) == nullptr);
    rig.gates().refresh(*rig.settings().snapshot());
}

TEST_CASE("the page's own copy: subtitle, search fields and controls", "[widget]") {
    PagesRig rig;
    const fv::SettingsPageSpec* spec = rig.context().settingsPages().page(dest::dictionary);
    REQUIRE(spec != nullptr);
    CHECK(spec->title == QStringLiteral("Dictionary"));
    CHECK(spec->subtitle ==
          QStringLiteral(
              "Names, jargon, and spellings OpenWhisper_CPP should get right every time."));
    REQUIRE(spec->searchFields.size() == 4);
    CHECK(spec->searchFields[0].target == "dictionary_composer_tile");
    CHECK(spec->searchFields[2].label == QStringLiteral("Steer the speech model"));
    CHECK(spec->controls.size() == 16);
    fv::SettingsDialog& dialog = rig.dialog();
    for (const std::string& name : spec->controls) {
        INFO(name);
        CHECK(dialog.control(name) != nullptr);
    }
}

TEST_CASE("an edited word that vanished on reload ends the edit, and removing it does too",
          "[widget]") {
    PagesRig rig(Json{{kKey, fourWords()}});
    fv::DictionaryPage& page = rig.dictionary();
    page.beginEdit(QStringLiteral("c"));
    CHECK(page.addButton()->text() == QStringLiteral("Save"));
    page.remove(QStringLiteral("c"));
    CHECK(page.addButton()->text() == QStringLiteral("Add"));
    CHECK(page.message()->text() == QStringLiteral("Removed “Kubernetes”."));

    page.beginEdit(QStringLiteral("a"));
    rig.settings().update(Json{{kKey, Json::array({word("z", "Zed")})}});
    page.refreshPage();
    CHECK(page.addButton()->text() == QStringLiteral("Add"));
    CHECK(page.cancelButton()->isHidden());
}

TEST_CASE("moving spelling rules says how many moved", "[widget]") {
    PagesRig rig(Json{{keys::transcript_cleanup_rules.key(),
                       Json::array({"Spell \"jon\" as \"John\".", "Use British spelling."})}});
    fv::DictionaryPage& page = rig.dictionary();
    CHECK_FALSE(page.rulesTile()->isHidden());
    CHECK(page.moveButton()->text() == QStringLiteral("Move it here"));
    page.moveButton()->click();
    CHECK(rig.saved(keys::transcript_cleanup_rules.key()) ==
          Json::array({"Use British spelling."}));
    CHECK(rig.terms() == std::vector<std::string>{"John"});
    CHECK(page.rulesTile()->isHidden());
    CHECK(page.message()->text() == QStringLiteral("Moved 1 spelling rule here."));
}
