// Settings > Personalize > Snippets: the switch, library, editor and Home row
// (tests/test_personalize_s5_settings_page.py), inside a real Settings window over an in-memory
// settings file. QMessageBox.question is the panel's `question` seam.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QAbstractButton>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>

#include "base/log.h"
#include "pages_support.h"
#include "settings/keys.h"
#include "text/personalize_copy.h"
#include "text/snippets.h"
#include "ui_personalize/snippets_page.h"
#include "ui_personalize/snippets_panel.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using fv::text::Snippet;
using namespace fvtest;

namespace {

const Snippet kCal{"cal", "my calendar link", "https://cal.example.com/dana", false};
const Snippet kAddress{"address", "my email address", "dana@example.com", false};
const Snippet kSignOff{"sig", "my sign-off", "**Dana Lee**\nDesigner", true};

Json library(const std::vector<Snippet>& snippets, Json values = Json::object()) {
    Json saved = Json::array();
    for (const Snippet& snippet : snippets) {
        saved.push_back(fv::text::snippet_to_json(snippet));
    }
    values[std::string(fv::keys::dictation_snippets.key())] = saved;
    return values;
}

// An answer to every question the panel asks, counting them.
void answer(fv::SnippetsPanel& panel, QMessageBox::StandardButton reply, int* asked = nullptr) {
    panel.question = [reply, asked](QWidget*, const QString&, const QString&,
                                    QMessageBox::StandardButtons, QMessageBox::StandardButton) {
        if (asked != nullptr) {
            ++*asked;
        }
        return reply;
    };
}

// Every record logged while it lives, at every level.
class CapturingSink : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        const std::lock_guard lock(mutex_);
        text_ += std::string(record.logger) + " " + std::string(record.message) + " " +
                 std::string(record.exception_type) + "\n";
    }
    std::string text() {
        const std::lock_guard lock(mutex_);
        return text_;
    }

private:
    std::mutex mutex_;
    std::string text_;
};

} // namespace

// from
// tests/test_personalize_s5_settings_page.py::test_rail_value_counts_snippets_and_says_when_off
TEST_CASE("test_rail_value_counts_snippets_and_says_when_off",
          "[test_personalize_s5_settings_page][unit]") {
    using fv::text::snippets_page::rail_value;
    CHECK(rail_value(Json::object()) == "None yet");
    CHECK(rail_value(library({kCal})) == "1 snippet");
    CHECK(rail_value(library({kCal, kAddress})) == "2 snippets");
    CHECK(rail_value(library({kCal}, Json{{"snippets_enabled", false}})) == "Off");
}

// from tests/test_personalize_s5_settings_page.py::test_the_switch_saves_and_tells_the_app
TEST_CASE("test_the_switch_saves_and_tells_the_app",
          "[test_personalize_s5_settings_page][widget]") {
    PagesRig rig(library({kCal}));
    fv::SnippetsPage& page = rig.snippetsPage();
    CHECK(page.enabledCheck()->isChecked());
    CHECK(rig.railValue(dest::snippets) == QStringLiteral("1 snippet"));
    page.enabledCheck()->click();
    CHECK(rig.saved(fv::keys::snippets_enabled.key()) == false);
    CHECK(page.enabledTile()->property("checked") == QVariant(false));
    CHECK(rig.railValue(dest::snippets) == QStringLiteral("Off"));
    CHECK(rig.changes == QStringList{QStringLiteral("snippets")});
}

// from
// tests/test_personalize_s5_settings_page.py::test_a_failed_switch_save_shows_what_is_still_saved
TEST_CASE("test_a_failed_switch_save_shows_what_is_still_saved",
          "[test_personalize_s5_settings_page][widget]") {
    PagesRig rig(library({kCal}));
    fv::SnippetsPage& page = rig.snippetsPage();
    rig.settings().file().fail_next_replace("Disk full");
    page.enabledCheck()->click();
    CHECK(page.enabledCheck()->isChecked());
    CHECK(page.enabledTile()->property("checked") == QVariant(true));
    CHECK(rig.dialog().messageText().contains(QStringLiteral("Disk full")));
    CHECK(rig.changes.isEmpty());
}

// from
// tests/test_personalize_s5_settings_page.py::test_the_page_has_its_tiles_and_shows_the_first_snippet
TEST_CASE("test_the_page_has_its_tiles_and_shows_the_first_snippet",
          "[test_personalize_s5_settings_page][widget]") {
    PagesRig rig(library({kCal, kSignOff}));
    fv::SnippetsPanel& panel = rig.snippets();
    QStringList titles;
    for (const auto* tile : rig.dialog().page(dest::snippets)->findChildren<fv::TileBase*>()) {
        titles << tile->titleLabel()->text();
    }
    CHECK(titles == QStringList{QStringLiteral("Expand snippets as you dictate"),
                                QStringLiteral("Your snippets")});
    CHECK(panel.countLabel()->text() == QStringLiteral("2 / 200"));
    REQUIRE(panel.snippetList()->count() == 2);
    CHECK(panel.snippetList()->item(0)->text() == qs(kCal.trigger));
    CHECK(panel.snippetList()->item(1)->text() == qs(kSignOff.trigger));
    CHECK(panel.triggerEdit()->text() == qs(kCal.trigger));
    CHECK(panel.textEdit()->toPlainText() == qs(kCal.text));
    CHECK_FALSE(panel.formatSwitch()->isChecked());
    CHECK(panel.preview()->text() ==
          QStringLiteral("Say “my calendar link” → inserts 28 characters"));

    panel.snippetList()->setCurrentRow(1);

    CHECK(panel.formatSwitch()->isChecked());
    CHECK(panel.preview()->text() ==
          QStringLiteral("Say “my sign-off” → inserts 17 characters with formatting"));
    CHECK_FALSE(panel.hasUnsavedChanges());
}

// from tests/test_personalize_s5_settings_page.py::test_a_new_snippet_is_saved_and_reported
TEST_CASE("test_a_new_snippet_is_saved_and_reported",
          "[test_personalize_s5_settings_page][widget]") {
    PagesRig rig(library({kCal}));
    fv::SnippetsPanel& panel = rig.snippets();
    panel.newSnippet();
    panel.triggerEdit()->setText(QStringLiteral("  My Sign-off "));
    panel.textEdit()->setPlainText(QStringLiteral("Best,\nDana\n"));
    panel.formatSwitch()->setChecked(true);
    CHECK(panel.saveSnippet());

    const auto saved = rig.savedSnippets();
    REQUIRE(saved.size() == 2);
    CHECK(saved[0].trigger == kCal.trigger);
    CHECK(saved[0].text == kCal.text);
    CHECK_FALSE(saved[0].formatted);
    CHECK(saved[1].trigger == "My Sign-off");
    CHECK(saved[1].text == "Best,\nDana");
    CHECK(saved[1].formatted);
    REQUIRE(panel.snippetList()->currentItem() != nullptr);
    CHECK(panel.snippetList()->currentItem()->text() == QStringLiteral("My Sign-off"));
    CHECK(panel.message()->text() == QStringLiteral("Saved. Say “My Sign-off” to insert it."));
    CHECK(rig.railValue(dest::snippets) == QStringLiteral("2 snippets"));
    CHECK(rig.changes == QStringList{QStringLiteral("snippets")});
}

// from
// tests/test_personalize_s5_settings_page.py::test_save_explains_what_is_missing_and_writes_nothing
TEST_CASE("test_save_explains_what_is_missing_and_writes_nothing",
          "[test_personalize_s5_settings_page][widget]") {
    PagesRig rig(library({kCal}));
    fv::SnippetsPanel& panel = rig.snippets();
    panel.newSnippet();
    panel.triggerEdit()->setText(QStringLiteral("my home address"));
    CHECK_FALSE(panel.saveSnippet());
    CHECK(panel.message()->text() == QStringLiteral("Add the text to insert."));
    CHECK(panel.message()->property("tone").toString() == QStringLiteral("error"));
    CHECK(rig.savedSnippets() == std::vector<Snippet>{kCal});
    CHECK(rig.changes.isEmpty());
}

// from
// tests/test_personalize_s5_settings_page.py::test_trigger_notes_flag_duplicates_and_near_misses
TEST_CASE("test_trigger_notes_flag_duplicates_and_near_misses",
          "[test_personalize_s5_settings_page][widget]") {
    PagesRig rig(library({kCal, kAddress}));
    fv::SnippetsPanel& panel = rig.snippets();
    panel.newSnippet();
    panel.triggerEdit()->setText(QStringLiteral("My Calendar-Link!"));
    CHECK(panel.triggerNote()->text() ==
          QStringLiteral("Another snippet already uses this trigger."));
    CHECK(panel.triggerNote()->property("tone").toString() == QStringLiteral("error"));
    panel.textEdit()->setPlainText(QStringLiteral("https://other.example.com"));
    CHECK_FALSE(panel.saveSnippet());
    CHECK(rig.savedSnippets().size() == 2);

    panel.triggerEdit()->setText(QStringLiteral("my email"));
    CHECK(
        panel.triggerNote()->text().startsWith(QStringLiteral("Also part of “my email address”")));
    CHECK(panel.triggerNote()->property("tone").toString() == QStringLiteral("hint"));
    CHECK_FALSE(panel.triggerNote()->isHidden());

    panel.triggerEdit()->setText(QStringLiteral("my office phone"));
    CHECK(panel.triggerNote()->isHidden());
}

// from
// tests/test_personalize_s5_settings_page.py::test_switching_away_from_unsaved_edits_asks_first
TEST_CASE("test_switching_away_from_unsaved_edits_asks_first",
          "[test_personalize_s5_settings_page][widget]") {
    PagesRig rig(library({kCal, kAddress}));
    fv::SnippetsPanel& panel = rig.snippets();
    panel.textEdit()->setPlainText(QStringLiteral("https://cal.example.com/new"));

    int asked = 0;
    answer(panel, QMessageBox::Cancel, &asked);
    panel.snippetList()->setCurrentRow(1);
    CHECK(asked == 1);
    CHECK(panel.snippetList()->currentRow() == 0);
    CHECK(panel.textEdit()->toPlainText() == QStringLiteral("https://cal.example.com/new"));

    answer(panel, QMessageBox::Save);
    panel.snippetList()->setCurrentRow(1);
    CHECK(panel.triggerEdit()->text() == qs(kAddress.trigger));
    CHECK(rig.savedSnippets()[0].text == "https://cal.example.com/new");

    panel.textEdit()->setPlainText(QStringLiteral("changed my mind"));
    answer(panel, QMessageBox::Discard);
    CHECK(panel.newSnippet());
    CHECK(rig.savedSnippets()[1] == kAddress);
    CHECK(panel.triggerEdit()->text().isEmpty());
}

// from tests/test_personalize_s5_settings_page.py::test_refreshing_settings_keeps_an_unsaved_draft
TEST_CASE("test_refreshing_settings_keeps_an_unsaved_draft",
          "[test_personalize_s5_settings_page][widget]") {
    PagesRig rig(library({kCal}));
    fv::SnippetsPanel& panel = rig.snippets();
    panel.newSnippet();
    panel.triggerEdit()->setText(QStringLiteral("my home address"));

    rig.dialog().refresh();

    CHECK(panel.triggerEdit()->text() == QStringLiteral("my home address"));
    CHECK(panel.hasUnsavedChanges());
}

// from
// tests/test_personalize_s5_settings_page.py::test_duplicate_starts_an_unsaved_copy_with_a_free_trigger
TEST_CASE("test_duplicate_starts_an_unsaved_copy_with_a_free_trigger",
          "[test_personalize_s5_settings_page][widget]") {
    PagesRig rig(library({kSignOff, Snippet{"two", "my sign-off 2", "x", false}}));
    fv::SnippetsPanel& panel = rig.snippets();

    panel.duplicateSnippet();

    CHECK(panel.triggerEdit()->text() == QStringLiteral("my sign-off 3"));
    CHECK(panel.textEdit()->toPlainText() == qs(kSignOff.text));
    CHECK(panel.formatSwitch()->isChecked());
    CHECK(panel.hasUnsavedChanges());
    CHECK(panel.snippetList()->currentRow() == -1);
    CHECK(rig.savedSnippets().size() == 2);
}

// from
// tests/test_personalize_s5_settings_page.py::test_delete_asks_then_removes_and_shows_the_next_snippet
TEST_CASE("test_delete_asks_then_removes_and_shows_the_next_snippet",
          "[test_personalize_s5_settings_page][widget]") {
    PagesRig rig(library({kCal, kAddress}));
    fv::SnippetsPanel& panel = rig.snippets();

    answer(panel, QMessageBox::Cancel);
    panel.deleteSnippet();
    CHECK(rig.savedSnippets().size() == 2);

    answer(panel, QMessageBox::Yes);
    panel.deleteSnippet();
    CHECK(rig.savedSnippets() == std::vector<Snippet>{kAddress});
    CHECK(panel.triggerEdit()->text() == qs(kAddress.trigger));
    CHECK(rig.railValue(dest::snippets) == QStringLiteral("1 snippet"));
    CHECK(rig.changes == QStringList{QStringLiteral("snippets")});
}

// from tests/test_personalize_s5_settings_page.py::test_an_empty_library_offers_starters
TEST_CASE("test_an_empty_library_offers_starters", "[test_personalize_s5_settings_page][widget]") {
    PagesRig rig;
    fv::SnippetsPanel& panel = rig.snippets();
    fv::SettingsDialog& dialog = rig.shownDialog();
    QCoreApplication::processEvents();

    CHECK(panel.emptyState()->isVisible());
    CHECK_FALSE(panel.snippetList()->isVisible());
    CHECK_FALSE(panel.deleteButton()->isEnabled());
    CHECK_FALSE(panel.duplicateButton()->isEnabled());
    fv::Button* starter = nullptr;
    for (fv::Button* button : panel.suggestionButtons()) {
        if (button->text() == QStringLiteral("my calendar link")) {
            starter = button;
        }
    }
    REQUIRE(starter != nullptr);
    starter->click();

    CHECK(panel.triggerEdit()->text() == QStringLiteral("my calendar link"));
    CHECK(pumpUntil([&panel] { return panel.textEdit()->hasFocus(); }));
    CHECK(panel.preview()->text() ==
          QStringLiteral("Say “my calendar link” → add the text it inserts."));
    CHECK(panel.hasUnsavedChanges());
    // Closing the visible window would ask about this unfinished draft.
    dialog.hide();
}

// from tests/test_personalize_s5_settings_page.py::test_snippet_text_is_never_logged
TEST_CASE("test_snippet_text_is_never_logged", "[test_personalize_s5_settings_page][widget]") {
    const std::string secret = "https://cal.example.com/zqsnip-token-123";
    PagesRig rig(library({kCal}));
    fv::SnippetsPanel& panel = rig.snippets();
    auto sink = std::make_shared<CapturingSink>();
    const fv::log::Level before = fv::log::level();
    fv::log::set_level(fv::log::Level::Debug);
    fv::log::add_sink(sink);
    panel.newSnippet();
    panel.triggerEdit()->setText(QStringLiteral("my zqsnip link"));
    panel.textEdit()->setPlainText(qs(secret));
    panel.saveSnippet();
    // A save that fails is logged by type alone.
    panel.textEdit()->setPlainText(qs(secret + "-again"));
    rig.settings().file().fail_next_replace("disk full");
    CHECK_FALSE(panel.saveSnippet());
    fv::log::remove_sink(sink);
    fv::log::set_level(before);
    CHECK(sink->text().find("zqsnip") == std::string::npos);
    CHECK(sink->text().find("Could not save the snippet") != std::string::npos);
}

// from tests/test_personalize_s5_settings_page.py::test_home_counts_snippets_and_adds_one
TEST_CASE("test_home_counts_snippets_and_adds_one", "[test_personalize_s5_settings_page][widget]") {
    PagesRig rig(library({kCal}));
    fv::SettingsDialog& dialog = rig.dialog();
    QPushButton* add = nullptr;
    for (auto* button : rig.home().findChildren<QPushButton*>()) {
        if (button->accessibleName() == QStringLiteral("Add a snippet")) {
            add = button;
        }
    }
    REQUIRE(add != nullptr);
    fv::WrappedLabel* detail = nullptr;
    for (auto* label :
         rig.home().findChildren<fv::WrappedLabel*>(QStringLiteral("homeRowDescription"))) {
        if (label->text() == QStringLiteral("1 snippet")) {
            detail = label;
        }
    }
    REQUIRE(detail != nullptr);
    CHECK_FALSE(dialog.isBuilt(dest::snippets));

    add->click();

    CHECK(dialog.rail()->currentKey() == qs(dest::snippets));
    fv::SnippetsPanel& panel = rig.snippets();
    CHECK(panel.triggerEdit()->text().isEmpty());
    CHECK(panel.snippetList()->currentRow() == -1);
    panel.triggerEdit()->setText(QStringLiteral("my email address"));
    panel.textEdit()->setPlainText(QStringLiteral("dana@example.com"));
    CHECK(panel.saveSnippet());
    // Home re-reads its rows when it shows again.
    dialog.showHome();
    CHECK(detail->text() == QStringLiteral("2 snippets"));

    // Switched off on the hidden page while Home is shown: the save's rail refresh redraws Home.
    auto* enabled = qobject_cast<QAbstractButton*>(dialog.control("snippets_enabled_check"));
    REQUIRE(enabled != nullptr);
    enabled->click();
    CHECK(dialog.rail()->currentKey() == qs(dest::overview));
    CHECK(detail->text() == QStringLiteral("Off · 2 snippets"));
}

// from tests/test_personalize_s5_settings_page.py::test_the_page_fits_narrow_windows_at_large_fonts
// The classic variant; the omarchy one is Linux's Omarchy desktop look, not ported.
TEST_CASE("test_the_page_fits_narrow_windows_at_large_fonts",
          "[test_personalize_s5_settings_page][widget]") {
    AppTheme theme(QStringLiteral("dark"), 130);
    PagesRig rig(library({kCal, kAddress, kSignOff}));
    fv::SnippetsPanel& panel = rig.snippets();
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    dialog.resize(940, 600);
    for (int i = 0; i < 8; ++i) {
        QCoreApplication::processEvents();
    }
    fv::SettingsPage* page = dialog.page(dest::snippets);
    CHECK(dialog.width() == 940);
    QList<QWidget*> controls;
    for (auto* button : page->findChildren<QAbstractButton*>()) {
        controls << button;
    }
    for (auto* field : page->findChildren<QLineEdit*>()) {
        controls << field;
    }
    for (QWidget* control : controls) {
        if (control->isVisible()) {
            INFO(control->objectName().toStdString());
            CHECK(control->mapTo(page, control->rect().topLeft()).x() >= 0);
            CHECK(control->mapTo(page, control->rect().bottomRight()).x() < page->width());
        }
    }
    for (auto* label : page->findChildren<QLabel*>()) {
        if (label->isVisible() && label->wordWrap()) {
            INFO(label->text().toStdString());
            CHECK(label->height() >= label->heightForWidth(label->width()));
        }
    }
    const std::vector<QPushButton*> buttons{panel.newButton(), panel.duplicateButton(),
                                            panel.deleteButton(), panel.saveButton()};
    for (std::size_t index = 0; index < buttons.size(); ++index) {
        const QRect box = buttons[index]->rect().translated(
            buttons[index]->mapTo(page, buttons[index]->rect().topLeft()));
        for (std::size_t other = index + 1; other < buttons.size(); ++other) {
            const QRect otherBox = buttons[other]->rect().translated(
                buttons[other]->mapTo(page, buttons[other]->rect().topLeft()));
            CHECK_FALSE(box.intersects(otherBox));
        }
        CHECK(buttons[index]->width() >= buttons[index]->minimumSizeHint().width());
    }
    CHECK(panel.textEdit()->height() >= panel.textEdit()->minimumHeight());
    QScrollArea* scroll = dialog.pageScroll(dest::snippets);
    REQUIRE(scroll != nullptr);
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    QCoreApplication::processEvents();
    CHECK(page->mapTo(scroll->viewport(), page->rect().bottomRight()).y() <=
          scroll->viewport()->height());
    dialog.hide();
}

// from
// tests/test_personalize_s5_settings_page.py::test_the_home_row_fits_narrow_windows_at_large_fonts
// The classic variant; the omarchy one is Linux's Omarchy desktop look, not ported.
TEST_CASE("test_the_home_row_fits_narrow_windows_at_large_fonts",
          "[test_personalize_s5_settings_page][widget]") {
    AppTheme theme(QStringLiteral("dark"), 130);
    PagesRig rig(library({kCal}));
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    dialog.fitToScreen();
    dialog.resize(720, 600);
    for (int i = 0; i < 8; ++i) {
        QCoreApplication::processEvents();
    }
    fv::HomePage& home = rig.home();
    QPushButton* add = nullptr;
    for (auto* button : home.findChildren<QPushButton*>()) {
        if (button->accessibleName() == QStringLiteral("Add a snippet")) {
            add = button;
        }
    }
    REQUIRE(add != nullptr);
    CHECK(add->isVisible());
    CHECK(add->mapTo(&home, add->rect().topLeft()).x() >= 0);
    CHECK(add->mapTo(&home, add->rect().bottomRight()).x() < home.width());
    CHECK(add->width() >= add->minimumSizeHint().width());
    dialog.hide();
}

// --- What the port adds around the page ---

TEST_CASE("leaving the page saves a valid draft and offers to discard one that is not",
          "[widget]") {
    PagesRig rig(library({kCal}));
    fv::SnippetsPage& page = rig.snippetsPage();
    fv::SnippetsPanel& panel = *page.panel();
    panel.newSnippet();
    panel.triggerEdit()->setText(QStringLiteral("my office phone"));
    panel.textEdit()->setPlainText(QStringLiteral("555 0100"));
    CHECK(page.saveDrafts(false));
    CHECK(rig.savedSnippets().size() == 2);

    panel.newSnippet();
    panel.triggerEdit()->setText(QStringLiteral("my home address"));
    // Without asking, a draft that cannot be saved stays where it is.
    CHECK(page.saveDrafts(false));
    CHECK(panel.triggerEdit()->text() == QStringLiteral("my home address"));
    // Asked, the user keeps editing (Cancel) or discards it.
    int asked = 0;
    answer(panel, QMessageBox::Cancel, &asked);
    CHECK_FALSE(page.saveDrafts(true));
    CHECK(asked == 1);
    answer(panel, QMessageBox::Discard);
    CHECK(page.saveDrafts(true));
    CHECK_FALSE(panel.hasUnsavedChanges());
    CHECK(rig.savedSnippets().size() == 2);
}

TEST_CASE("the panel stacks its editor under the library when narrow", "[widget]") {
    fv::MemorySettingsStore store(
        library({kCal, kAddress, kSignOff, Snippet{"x", "my phone", "555", false}}));
    fv::SnippetsPanel panel(store);
    panel.resize(900, 500);
    panel.show();
    QCoreApplication::processEvents();
    auto* library = panel.findChild<QWidget*>(QStringLiteral("snippetsLibrary"));
    REQUIRE(library != nullptr);
    CHECK(library->maximumWidth() == fv::SnippetsPanel::kLibraryWidth);
    panel.resize(500, 600);
    QCoreApplication::processEvents();
    CHECK(library->minimumWidth() == 0);
    // About three and a half rows, scrolling.
    CHECK(panel.snippetList()->maximumHeight() == panel.snippetList()->minimumHeight());
    CHECK(panel.snippetList()->maximumHeight() < 4 * panel.snippetList()->sizeHintForRow(0) + 12);
    panel.hide();
}
