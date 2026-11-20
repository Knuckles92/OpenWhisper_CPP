// The card's right-click menu and the sidebar's header menu (history_sidebar.py
// _show_context_menu, _show_header_menu and their actions), the copies, and the edge tab.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <QAction>
#include <QDir>
#include <QMenu>
#include <QMessageBox>

#include "history_ui_support.h"
#include "ui_history/history_edge_tab.h"
#include "ui_history/history_item.h"
#include "ui_history/history_sidebar.h"

using fv::HistoryEntry;
using fv::HistoryItemWidget;
using fv::HistorySidebar;
using fvtest::FakeStore;
using fvtest::Io;

namespace {

// The menu's items in order, a separator as "-".
QStringList itemsOf(QMenu& menu) {
    QStringList items;
    for (const QAction* action : menu.actions()) {
        items << (action->isSeparator() ? QStringLiteral("-") : action->text());
    }
    return items;
}

QAction* actionOf(QMenu& menu, const QString& text) {
    for (QAction* action : menu.actions()) {
        if (action->text() == text) {
            return action;
        }
    }
    return nullptr;
}

struct SidebarFixture {
    SidebarFixture() : sidebar(store, io.executor(), nullptr) {
        sidebar.copyText = [this](const QString& value) {
            clipboard.push_back(value);
            return true;
        };
    }

    std::shared_ptr<FakeStore> store = std::make_shared<FakeStore>();
    Io io;
    HistorySidebar sidebar;
    std::vector<QString> clipboard;
};

} // namespace

TEST_CASE("a cleaned entry's menu offers both copies and the undo", "[widget]") {
    const fvtest::FakeDevice device;
    FakeStore store;
    store.recordings["a.wav"] = std::filesystem::path("a.wav");
    HistoryEntry entry = fvtest::makeEntry("Hello.");
    entry.raw_text = "hello";
    entry.audio_file = "a.wav";
    HistoryItemWidget card(entry, store);
    std::unique_ptr<QMenu> menu(card.buildContextMenu());

    CHECK(itemsOf(*menu) == QStringList{QStringLiteral("Copy"), QStringLiteral("Copy Raw"),
                                        QStringLiteral("Undo AI edit"),
                                        QStringLiteral("Transcribe again"),
                                        QStringLiteral("Show in Folder"), QStringLiteral("-"),
                                        QStringLiteral("Delete")});
}

TEST_CASE("a plain entry copies its text; a failed recording has nothing to copy", "[widget]") {
    FakeStore store;
    HistoryItemWidget plain(fvtest::makeEntry("As said."), store);
    std::unique_ptr<QMenu> plainMenu(plain.buildContextMenu());
    CHECK(itemsOf(*plainMenu) == QStringList{QStringLiteral("Copy Text"),
                                             QStringLiteral("Show in Folder"), QStringLiteral("-"),
                                             QStringLiteral("Delete")});

    HistoryEntry failed = fvtest::makeEntry("");
    failed.entry_kind = std::string(fv::FAILED_RECORDING_KIND);
    HistoryItemWidget failedCard(failed, store);
    std::unique_ptr<QMenu> failedMenu(failedCard.buildContextMenu());
    CHECK(itemsOf(*failedMenu) == QStringList{QStringLiteral("Show in Folder"), QStringLiteral("-"),
                                              QStringLiteral("Delete")});
}

TEST_CASE("the menu's actions ask for the entry's copy, version and delete", "[widget]") {
    FakeStore store;
    HistoryEntry entry = fvtest::makeEntry("hello");
    entry.raw_text = "hello";
    entry.cleaned_text = "Hello.";
    HistoryItemWidget card(entry, store);
    QStringList asked;
    const QString id = fvtest::q(entry.id);
    QObject::connect(&card, &HistoryItemWidget::copyRequested,
                     [&asked](const QString& value) { asked << QStringLiteral("copy:") + value; });
    QObject::connect(&card, &HistoryItemWidget::copyRawRequested,
                     [&asked](const QString& value) { asked << QStringLiteral("raw:") + value; });
    QObject::connect(&card, &HistoryItemWidget::versionRequested,
                     [&asked](const QString& value, const QString& version) {
                         asked << value + QStringLiteral(">") + version;
                     });
    QObject::connect(&card, &HistoryItemWidget::deleteRequested, [&asked](const QString& value) {
        asked << QStringLiteral("delete:") + value;
    });
    std::unique_ptr<QMenu> menu(card.buildContextMenu());
    for (const char* text : {"Copy", "Copy Raw", "Use AI version", "Delete"}) {
        QAction* action = actionOf(*menu, QString::fromLatin1(text));
        REQUIRE(action != nullptr);
        action->trigger();
    }

    CHECK(asked == QStringList{QStringLiteral("copy:") + id, QStringLiteral("raw:") + id,
                               id + QStringLiteral(">ai"), QStringLiteral("delete:") + id});
}

TEST_CASE("copies from the list put the entry's text on the clipboard", "[widget]") {
    SidebarFixture fixture;
    HistoryEntry entry = fvtest::makeEntry("Hello.");
    entry.id = "entry-1";
    entry.raw_text = "hello";
    fixture.store->entries[entry.id] = entry;
    std::vector<QString> copied;
    QObject::connect(&fixture.sidebar, &HistorySidebar::entryCopied,
                     [&copied](const QString& id) { copied.push_back(id); });

    fixture.sidebar.onCopyRequested(QStringLiteral("entry-1"));
    fixture.sidebar.onCopyRawRequested(QStringLiteral("entry-1"));
    fixture.sidebar.onCopyRequested(QStringLiteral("gone"));
    fixture.io.drain();

    CHECK(fixture.clipboard ==
          std::vector<QString>{QStringLiteral("Hello."), QStringLiteral("hello")});
    CHECK(copied == std::vector<QString>(2, QStringLiteral("entry-1")));
}

TEST_CASE("an entry that is gone opens nothing", "[widget]") {
    SidebarFixture fixture;
    fixture.store->entries["entry-1"] = fvtest::makeEntry("here");
    std::vector<QString> selected;
    QObject::connect(&fixture.sidebar, &HistorySidebar::entrySelected,
                     [&selected](const QString& id) { selected.push_back(id); });

    fixture.sidebar.onEntryClicked(QStringLiteral("entry-1"));
    fixture.sidebar.onEntryClicked(QStringLiteral("gone"));
    fixture.io.drain();

    CHECK(selected == std::vector<QString>{QStringLiteral("entry-1")});
}

TEST_CASE("the header menu lists export, the folder and the two clears", "[widget]") {
    SidebarFixture fixture;
    std::unique_ptr<QMenu> menu(fixture.sidebar.buildHeaderMenu());
    CHECK(itemsOf(*menu) == QStringList{QStringLiteral("Export history…"),
                                        QStringLiteral("Open recordings folder"),
                                        QStringLiteral("-"), QStringLiteral("Clear history"),
                                        QStringLiteral("Clear history + recordings")});
    int exports = 0;
    QObject::connect(&fixture.sidebar, &HistorySidebar::exportRequested, [&exports] { ++exports; });
    actionOf(*menu, QStringLiteral("Export history…"))->trigger();
    CHECK(exports == 1);
}

TEST_CASE("clearing history asks first, No by default", "[widget]") {
    SidebarFixture fixture;
    QStringList asked;
    QMessageBox::StandardButton answer = QMessageBox::No;
    fixture.sidebar.runMessageBox = [&](QMessageBox& box) {
        asked << box.windowTitle() + QStringLiteral("|") + box.text();
        CHECK(box.icon() == QMessageBox::Question);
        CHECK(box.defaultButton() == box.button(QMessageBox::No));
        return int(answer);
    };

    fixture.sidebar.onClearHistory();
    fixture.io.drain();
    CHECK(fixture.store->clears() == 0);

    answer = QMessageBox::Yes;
    fixture.sidebar.onClearHistory();
    fixture.sidebar.onClearHistoryAndRecordings();
    fixture.io.drain();
    CHECK(fixture.store->clears() == 2);

    CHECK(asked ==
          QStringList{QStringLiteral("Clear History|Delete all history entries?\n\nSaved "
                                     "recordings will be kept."),
                      QStringLiteral("Clear History|Delete all history entries?\n\nSaved "
                                     "recordings will be kept."),
                      QStringLiteral("Clear History and Recordings|Delete all history entries "
                                     "AND permanently delete all saved recordings from "
                                     "disk?\n\nThis cannot be undone.")});
}

TEST_CASE("the recordings folder is made and opened", "[widget]") {
    SidebarFixture fixture;
    const fv::testkit::TempDataRoot root;
    fixture.store->folder = root.path() / "recordings";
    std::vector<QString> opened;
    fixture.sidebar.openFolder = [&opened](const QString& folder) {
        opened.push_back(folder);
        return true;
    };

    fixture.sidebar.onOpenRecordingsFolder();

    CHECK(std::filesystem::is_directory(fixture.store->folder));
    REQUIRE(opened.size() == 1);
    CHECK(QDir::cleanPath(opened[0]) ==
          QDir::cleanPath(QString::fromStdWString(fixture.store->folder.wstring())));
}

TEST_CASE("a version chosen from the menu refreshes the list", "[widget]") {
    fvtest::LocalHistory history;
    const HistoryEntry entry = history.addCleaned();
    Io io;
    HistorySidebar sidebar(history.store(), io.executor(), nullptr);
    sidebar.expand();
    io.drain();
    REQUIRE(sidebar.renderedCardCount() == 1);
    const QWidget* before = sidebar.historyCards().at(entry.id).card;

    sidebar.onVersionRequested(fvtest::q(entry.id), QStringLiteral("original"));
    io.drain();

    // The entry changed, so its card was made again.
    CHECK(sidebar.historyCards().at(entry.id).card != before);
    CHECK(history.get(entry.id)->text == "um hello world");
}

TEST_CASE("the edge tab names the panel it opens", "[widget]") {
    fv::HistoryEdgeTab tab;
    CHECK(tab.objectName() == QStringLiteral("historyEdgeTab"));
    CHECK(tab.width() == 24);
    CHECK(tab.minimumHeight() == 80);
    CHECK(tab.text() == QStringLiteral("‹"));
    CHECK(tab.toolTip() == QStringLiteral("Open History"));
    tab.setShortcutHint(QStringLiteral("Ctrl+H"));
    tab.setPanelName(QStringLiteral("Past Meetings"));
    CHECK(tab.toolTip() == QStringLiteral("Open Past Meetings (Ctrl+H)"));
    tab.setExpanded(true);
    CHECK(tab.text() == QStringLiteral("›"));
    CHECK(tab.toolTip() == QStringLiteral("Close Past Meetings (Ctrl+H)"));
    tab.setShortcutHint(QString());
    CHECK(tab.toolTip() == QStringLiteral("Close Past Meetings"));
}

TEST_CASE("the sidebar slides between 0 and 380 pixels", "[widget]") {
    SidebarFixture fixture;
    HistorySidebar& sidebar = fixture.sidebar;
    CHECK(sidebar.maximumWidth() == 0);
    std::vector<int> frames;
    QObject::connect(&sidebar, &HistorySidebar::widthAnimated,
                     [&frames](int width) { frames.push_back(width); });

    sidebar.toggle();
    CHECK(sidebar.isExpanded());
    REQUIRE(fvtest::waitFor(
        [&sidebar] { return sidebar.animation()->state() == QAbstractAnimation::Stopped; }));
    CHECK(sidebar.width() == HistorySidebar::kExpandedWidth);
    CHECK(sidebar.minimumWidth() == HistorySidebar::kExpandedWidth);
    REQUIRE_FALSE(frames.empty());
    CHECK(frames.back() == HistorySidebar::kExpandedWidth);
    CHECK(sidebar.animation()->duration() == 300);
    CHECK(sidebar.animation()->easingCurve().type() == QEasingCurve::InOutCubic);

    sidebar.toggle();
    REQUIRE(fvtest::waitFor(
        [&sidebar] { return sidebar.animation()->state() == QAbstractAnimation::Stopped; }));
    CHECK_FALSE(sidebar.isExpanded());
    CHECK(sidebar.maximumWidth() == 0);
    CHECK(frames.back() == 0);
}

TEST_CASE("a refresh while collapsed waits for the next open", "[widget]") {
    SidebarFixture fixture;
    int loads = 0;
    fixture.store->onGetHistory = [&loads](std::optional<std::int64_t>) {
        ++loads;
        return std::vector<HistoryEntry>{};
    };
    fixture.sidebar.refresh();
    fixture.io.drain();
    CHECK(loads == 0);
    CHECK(fixture.sidebar.refreshPending());
    fixture.sidebar.expand();
    fixture.io.drain();
    CHECK(loads == 1);
    CHECK_FALSE(fixture.sidebar.refreshPending());
}
