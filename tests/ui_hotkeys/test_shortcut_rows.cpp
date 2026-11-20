// The Walkthroughs page's copies of the standard shortcut rows
// (settings_dialog.py _hotkey_shortcut_row(page=...), _add_walkthrough_shortcut_rows), from
// tests/test_hotkey_directory.py::test_walkthrough_shortcuts_agree_on_both_pages. The rows are
// built through the one HotkeysPage, so they share its capture, conflict check and Reset to
// defaults.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include <QVBoxLayout>

#include "hotkeys_test_support.h"
#include "ui_hotkeys/standard_hotkeys.h"
#include "ui_widgets/buttons.h"

using fv::Json;
using namespace fvtest;

namespace {

constexpr std::string_view kWalkthroughs = "walkthroughs";

// A stand-in for the Walkthroughs page: the three rows, laid out in a widget of its own.
struct OtherPage {
    explicit OtherPage(fv::HotkeysPage& hotkeys) : root(std::make_unique<QWidget>()) {
        auto* layout = new QVBoxLayout(root.get());
        for (const auto& item : fv::standard_hotkeys::walkthroughRows()) {
            layout->addWidget(hotkeys.shortcutRow(item.key, QString::fromStdString(item.title),
                                                  QString::fromStdString(item.description),
                                                  !item.required, kWalkthroughs));
        }
    }

    std::unique_ptr<QWidget> root;
};

} // namespace

// from tests/test_hotkey_directory.py::test_walkthrough_shortcuts_agree_on_both_pages
TEST_CASE("walkthrough shortcuts agree on both pages", "[test_hotkey_directory]") {
    PageScene scene;
    auto& page = *scene.page;
    OtherPage other(page);
    other.root->show();
    page.show();
    auto* onWalkthroughs = page.shortcutInput("walkthrough", kWalkthroughs);
    REQUIRE(onWalkthroughs != nullptr);
    QTest::mouseClick(onWalkthroughs, Qt::LeftButton);
    CHECK(page.capturingKey() == std::optional<std::string>("walkthrough"));
    REQUIRE(scene.capture() != nullptr);

    scene.capture()->capture("ctrl+alt+w");

    CHECK(scene.saved("walkthrough") == "ctrl+alt+w");
    const QString shown = display("ctrl+alt+w");
    CHECK(onWalkthroughs->text() == shown);
    CHECK(page.hotkeyInput("walkthrough")->text() == shown);

    // Clear on Hotkeys empties both.
    page.clearButton("walkthrough")->click();
    CHECK(scene.saved("walkthrough") == "");
    CHECK(onWalkthroughs->text().isEmpty());
    CHECK(page.hotkeyInput("walkthrough")->text().isEmpty());
}

TEST_CASE("the Walkthroughs page's rows are not the Hotkeys page's",
          "[walkthrough_shortcut_rows]") {
    PageScene scene;
    auto& page = *scene.page;
    CHECK(page.shortcutInput("walkthrough", kWalkthroughs) == nullptr);
    OtherPage other(page);
    for (const char* key : {"paste_walkthrough", "walkthrough", "screenshot"}) {
        INFO(key);
        auto* mine = page.shortcutInput(key, kWalkthroughs);
        auto* theirs = page.hotkeyInput(key);
        REQUIRE(mine != nullptr);
        REQUIRE(theirs != nullptr);
        CHECK(mine != theirs);
        CHECK(other.root->isAncestorOf(mine));
        CHECK_FALSE(other.root->isAncestorOf(theirs));
        CHECK(page.isAncestorOf(theirs));
    }
    // The Hotkeys accessors never name a row of another page, and an action with no row there has
    // no field.
    CHECK(page.shortcutInput("record_toggle", kWalkthroughs) == nullptr);
    CHECK(page.shortcutInput("walkthrough", "elsewhere") == nullptr);
    CHECK(page.shortcutInput("walkthrough") == page.hotkeyInput("walkthrough"));
}

TEST_CASE("the three rows are the Hotkeys page's, word for word", "[walkthrough_shortcut_rows]") {
    const auto rows = fv::standard_hotkeys::walkthroughRows();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].key == "paste_walkthrough");
    CHECK(rows[0].title == "Paste latest walkthrough");
    CHECK(rows[1].key == "walkthrough");
    CHECK(rows[1].title == "Walkthrough");
    CHECK(rows[2].key == "screenshot");
    CHECK(rows[2].title == "Screenshot");
    for (const auto& row : rows) {
        CHECK(row.kind == fv::standard_hotkeys::Item::Kind::Row);
        CHECK_FALSE(row.required);
    }
    // The Hotkeys page lists them as its Walkthroughs group.
    const auto items = fv::standard_hotkeys::items();
    std::size_t at = 0;
    while (at < items.size() && items[at].title != "Walkthroughs") {
        ++at;
    }
    REQUIRE(at + 3 < items.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        CHECK(items[at + 1 + i].key == rows[i].key);
        CHECK(items[at + 1 + i].title == rows[i].title);
        CHECK(items[at + 1 + i].description == rows[i].description);
    }
}

TEST_CASE("a row built after the page loaded shows the saved shortcut",
          "[walkthrough_shortcut_rows]") {
    PageScene scene(Json{{"hotkeys", {{"walkthrough", "ctrl+alt+w"}, {"screenshot", ""}}}});
    auto& page = *scene.page;
    OtherPage other(page);
    CHECK(page.shortcutInput("walkthrough", kWalkthroughs)->text() == display("ctrl+alt+w"));
    CHECK(page.shortcutInput("screenshot", kWalkthroughs)->text().isEmpty());
    // An optional row's Clear is off while the shortcut is empty, as on the Hotkeys page.
    const auto clear = other.root->findChildren<fv::Button*>(QStringLiteral("hotkeyClearButton"));
    REQUIRE(clear.size() == 3);
    CHECK_FALSE(clear[2]->isEnabled());
    CHECK(clear[1]->isEnabled());

    // Saved from elsewhere, then read again: every row follows.
    scene.store->save_all(Json{{"hotkeys", {{"walkthrough", "f9"}}}});
    page.reloadShortcuts();
    CHECK(page.shortcutInput("walkthrough", kWalkthroughs)->text() == display("f9"));
    CHECK(page.hotkeyInput("walkthrough")->text() == display("f9"));
}

TEST_CASE("a refused shortcut shows its note on both rows", "[walkthrough_shortcut_rows]") {
    PageScene scene(Json{{"hotkeys", {{"cancel", "f8"}}}});
    auto& page = *scene.page;
    OtherPage other(page);
    page.show();
    other.root->show();
    QTest::mouseClick(page.shortcutInput("screenshot", kWalkthroughs), Qt::LeftButton);
    REQUIRE(scene.capture() != nullptr);
    scene.capture()->capture("f8");
    // Nothing was saved, the message names the owner, and both rows say so.
    CHECK(scene.saved("screenshot").empty());
    CHECK(scene.host->message().contains(QStringLiteral("already used by")));
    CHECK(scene.note("screenshot").find("already used by") != std::string::npos);
    auto notes = other.root->findChildren<QLabel*>(QStringLiteral("hotkeyShortcutNote"));
    REQUIRE(notes.size() == 3);
    CHECK_FALSE(notes[2]->isHidden());
    CHECK(notes[2]->text().contains(QStringLiteral("already used by")));
}

TEST_CASE("a row whose page went first leaves the Hotkeys page working",
          "[walkthrough_shortcut_rows]") {
    PageScene scene;
    auto& page = *scene.page;
    auto other = std::make_unique<OtherPage>(page);
    QTest::mouseClick(page.shortcutInput("walkthrough", kWalkthroughs), Qt::LeftButton);
    REQUIRE(page.capturingKey().has_value());
    other.reset();
    CHECK(page.shortcutInput("walkthrough", kWalkthroughs) == nullptr);

    // Its capture finishing, and every later redraw, pass over the rows that are gone.
    page.cancelCapture();
    scene.store->save_all(Json{{"hotkeys", {{"walkthrough", "f9"}}}});
    page.loadSettings(*scene.store->snapshot());
    CHECK(page.hotkeyInput("walkthrough")->text() == display("f9"));
}
