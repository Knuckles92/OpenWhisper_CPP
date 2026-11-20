// The main-window tab container: saved and restored selection, recording lock, legacy saved ids.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>

#include <QApplication>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTabBar>
#include <QTest>
#include <QTimer>

#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/tabbed_content.h"

using fv::TabbedContentWidget;

namespace {

constexpr int kTabs[] = {0, 1, 2, 3};

std::unique_ptr<TabbedContentWidget> makeTabs(fv::SettingsStore& store) {
    auto tabs = std::make_unique<TabbedContentWidget>(store);
    for (int i = 0; i < 4; ++i) {
        tabs->setPage(i, new QWidget);
    }
    tabs->syncStackWithTabBar();
    tabs->resize(720, 240);
    tabs->show();
    QApplication::processEvents();
    CHECK(QTest::qWaitForWindowExposed(tabs.get()));
    tabs->activateWindow();
    QApplication::processEvents();
    return tabs;
}

void saveTab(fv::SettingsStore& store, int savedId) {
    store.save_setting(fv::keys::last_tab_index.key(), savedId);
}

fv::Json savedTab(const fv::SettingsStore& store) {
    return store.get(fv::keys::last_tab_index);
}

QTimer* saveTimer(TabbedContentWidget& tabs) {
    return tabs.findChild<QTimer*>(QString(), Qt::FindDirectChildrenOnly);
}

} // namespace

// from tests/test_tabbed_content.py::test_user_tab_selection_is_saved_and_restored
TEST_CASE("user tab selection is saved and restored", "[test_tabbed_content][widget]") {
    const int selectedTab = GENERATE(0, 1, 2, 3);
    fv::MemorySettingsStore store;
    saveTab(store, TabbedContentWidget::savedTabId((selectedTab + 1) % 4));
    const auto tabs = makeTabs(store);

    QSignalSpy saved(saveTimer(*tabs), &QTimer::timeout);
    QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier,
                      tabs->tabBar()->tabRect(selectedTab).center());
    CHECK(tabs->currentIndex() == selectedTab);
    CHECK((saved.count() > 0 || saved.wait(2000)));

    CHECK(savedTab(store) == fv::Json(TabbedContentWidget::savedTabId(selectedTab)));
    const auto reopened = makeTabs(store);
    CHECK(reopened->currentIndex() == selectedTab);
    CHECK(reopened->stack()->currentIndex() == selectedTab);
}

// from tests/test_tabbed_content.py::test_recording_selects_source_without_visiting_other_tabs
TEST_CASE("recording selects source without visiting other tabs", "[test_tabbed_content][widget]") {
    const int previousTab = GENERATE(0, 1, 2, 3);
    const int sourceTab = GENERATE(0, 1, 2, 3);
    fv::MemorySettingsStore store;
    saveTab(store, TabbedContentWidget::savedTabId(previousTab));
    const auto tabs = makeTabs(store);
    std::vector<int> changes;
    QObject::connect(tabs.get(), &TabbedContentWidget::currentChanged,
                     [&changes](int index) { changes.push_back(index); });

    tabs->setRecordingState(true, sourceTab);
    CHECK(changes == (previousTab == sourceTab ? std::vector<int>{} : std::vector<int>{sourceTab}));
    CHECK(tabs->currentIndex() == sourceTab);
    CHECK(tabs->stack()->currentIndex() == sourceTab);
    for (const int i : kTabs) {
        CHECK(tabs->tabBar()->isTabEnabled(i) == (i == sourceTab));
    }

    tabs->setRecordingState(false, -1);
    tabs->flushPendingTabSave();
    CHECK(tabs->currentIndex() == sourceTab);
    for (const int i : kTabs) {
        CHECK(tabs->tabBar()->isTabEnabled(i));
    }
    CHECK(savedTab(store) == fv::Json(TabbedContentWidget::savedTabId(sourceTab)));
}

// from tests/test_tabbed_content.py::test_a_tab_saved_before_walkthrough_joined_still_opens
TEST_CASE("a tab saved before walkthrough joined still opens", "[test_tabbed_content][widget]") {
    // Meeting Mode was saved as 2 when it was the third tab; it is fourth now.
    const auto [saved, tab] = GENERATE(std::pair{0, TabbedContentWidget::TabQuickRecord},
                                       std::pair{1, TabbedContentWidget::TabUploadFile},
                                       std::pair{2, TabbedContentWidget::TabMeetingMode},
                                       std::pair{3, TabbedContentWidget::TabWalkthrough});
    fv::MemorySettingsStore store;
    saveTab(store, saved);
    CHECK(makeTabs(store)->currentIndex() == tab);
}

// from tests/test_tabbed_content.py::test_meeting_mode_stays_last_for_the_themes_last_tab_rules
TEST_CASE("meeting mode stays last for the themes last tab rules",
          "[test_tabbed_content][widget]") {
    fv::MemorySettingsStore store;
    const auto tabs = makeTabs(store);
    CHECK(TabbedContentWidget::TabMeetingMode == tabs->tabBar()->count() - 1);
    CHECK(tabs->tabBar()->tabText(TabbedContentWidget::TabMeetingMode) ==
          QStringLiteral("Meeting Mode"));
    CHECK(tabs->tabBar()->tabText(TabbedContentWidget::TabWalkthrough) ==
          QStringLiteral("Walkthrough"));
}

TEST_CASE("the tab constants and the legacy saved ids", "[widget]") {
    CHECK(TabbedContentWidget::TabQuickRecord == 0);
    CHECK(TabbedContentWidget::TabUploadFile == 1);
    CHECK(TabbedContentWidget::TabWalkthrough == 2);
    CHECK(TabbedContentWidget::TabMeetingMode == 3);
    CHECK(TabbedContentWidget::savedTabId(TabbedContentWidget::TabQuickRecord) == 0);
    CHECK(TabbedContentWidget::savedTabId(TabbedContentWidget::TabUploadFile) == 1);
    CHECK(TabbedContentWidget::savedTabId(TabbedContentWidget::TabMeetingMode) == 2);
    CHECK(TabbedContentWidget::savedTabId(TabbedContentWidget::TabWalkthrough) == 3);
}

TEST_CASE("a saved value matches by Python equality, and anything else opens Quick Record",
          "[widget]") {
    const auto [stored, tab] =
        GENERATE(std::pair{fv::Json(true), TabbedContentWidget::TabUploadFile},
                 std::pair{fv::Json(false), TabbedContentWidget::TabQuickRecord},
                 std::pair{fv::Json(1.0), TabbedContentWidget::TabUploadFile},
                 std::pair{fv::Json(2.0), TabbedContentWidget::TabMeetingMode},
                 std::pair{fv::Json("1"), TabbedContentWidget::TabQuickRecord},
                 std::pair{fv::Json(nullptr), TabbedContentWidget::TabQuickRecord},
                 std::pair{fv::Json(7), TabbedContentWidget::TabQuickRecord},
                 std::pair{fv::Json::array({1}), TabbedContentWidget::TabQuickRecord});
    fv::MemorySettingsStore store;
    store.save_setting(fv::keys::last_tab_index.key(), stored);
    CHECK(makeTabs(store)->currentIndex() == tab);
}

TEST_CASE("the selection is saved 250 ms after a change and not at all when it returns to the "
          "saved tab",
          "[widget]") {
    fv::MemorySettingsStore store;
    saveTab(store, 0);
    const auto tabs = makeTabs(store);

    tabs->setCurrentIndex(fv::TabbedContentWidget::TabUploadFile);
    CHECK(saveTimer(*tabs)->isActive());
    CHECK(saveTimer(*tabs)->interval() == 250);
    CHECK(savedTab(store) == fv::Json(0));

    // Back on the saved tab before the timer fires: nothing left to save.
    tabs->setCurrentIndex(fv::TabbedContentWidget::TabQuickRecord);
    CHECK_FALSE(saveTimer(*tabs)->isActive());
    tabs->flushPendingTabSave();
    CHECK(savedTab(store) == fv::Json(0));

    tabs->setCurrentIndex(fv::TabbedContentWidget::TabWalkthrough);
    tabs->flushPendingTabSave();
    CHECK(savedTab(store) == fv::Json(3));
    CHECK_FALSE(saveTimer(*tabs)->isActive());
}

TEST_CASE("selecting a tab shows its page and tells the owner", "[widget]") {
    fv::MemorySettingsStore store;
    const auto tabs = makeTabs(store);
    QSignalSpy changed(tabs.get(), &TabbedContentWidget::currentChanged);
    tabs->setCurrentIndex(2);
    CHECK(tabs->stack()->currentIndex() == 2);
    REQUIRE(changed.count() == 1);
    CHECK(changed.takeFirst().at(0).toInt() == 2);
    // An index out of range is ignored.
    tabs->setCurrentIndex(9);
    tabs->setCurrentIndex(-1);
    CHECK(tabs->currentIndex() == 2);
    CHECK(changed.count() == 0);
}

TEST_CASE("setPage replaces a page and fills gaps with empty ones", "[widget]") {
    fv::MemorySettingsStore store;
    TabbedContentWidget tabs(store);
    auto* third = new QWidget;
    tabs.setPage(2, third);
    CHECK(tabs.stack()->count() == 3);
    CHECK(tabs.stack()->widget(2) == third);

    auto* replacement = new QWidget;
    tabs.setPage(2, replacement);
    CHECK(tabs.stack()->count() == 3);
    CHECK(tabs.stack()->widget(2) == replacement);
    CHECK(tabs.tabBar()->objectName() == QStringLiteral("contentTabBar"));
    CHECK(tabs.stack()->objectName() == QStringLiteral("contentStack"));
}

TEST_CASE("a locked Meeting Mode tab cannot be selected and tells the owner", "[widget]") {
    fv::MemorySettingsStore store;
    const auto tabs = makeTabs(store);
    CHECK_FALSE(tabs->meetingTabIsLocked());
    CHECK(tabs->tabBar()->property(fv::ui::prop::unsupportedMeeting).toBool() == false);
    CHECK(tabs->tabBar()->tabToolTip(TabbedContentWidget::TabMeetingMode) ==
          QStringLiteral("Meeting Mode is in beta. Transcripts and insights may be inaccurate."));

    tabs->setMeetingTabLocked(true, QStringLiteral("Linux"));
    CHECK(tabs->meetingTabIsLocked());
    CHECK(tabs->tabBar()->property(fv::ui::prop::unsupportedMeeting).toBool());
    CHECK(tabs->tabBar()->tabToolTip(TabbedContentWidget::TabMeetingMode) ==
          QStringLiteral("Meeting Mode is not supported on Linux. Click to continue anyway."));

    QSignalSpy activated(tabs.get(), &TabbedContentWidget::lockedMeetingTabActivated);
    tabs->setCurrentIndex(TabbedContentWidget::TabUploadFile);
    QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier,
                      tabs->tabBar()->tabRect(TabbedContentWidget::TabMeetingMode).center());
    CHECK(activated.count() == 1);
    CHECK(tabs->currentIndex() == TabbedContentWidget::TabUploadFile);

    // Reached any other way than the user's click, the selection falls back to the last tab
    // that was allowed.
    tabs->tabBar()->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
    CHECK(tabs->currentIndex() == TabbedContentWidget::TabUploadFile);
    CHECK(activated.count() == 2);

    tabs->unlockMeetingTab();
    CHECK_FALSE(tabs->meetingTabIsLocked());
    CHECK(tabs->tabBar()->tabToolTip(TabbedContentWidget::TabMeetingMode) ==
          QStringLiteral("Meeting Mode is unsupported on Linux; system audio is unavailable."));
    tabs->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
    CHECK(tabs->currentIndex() == TabbedContentWidget::TabMeetingMode);
}

TEST_CASE("a saved Meeting Mode falls back to Quick Record when the tab gets locked", "[widget]") {
    fv::MemorySettingsStore store;
    saveTab(store, 2);
    TabbedContentWidget tabs(store);
    REQUIRE(tabs.currentIndex() == TabbedContentWidget::TabMeetingMode);
    tabs.setMeetingTabLocked(true, QStringLiteral("Linux"));
    CHECK(tabs.currentIndex() == TabbedContentWidget::TabQuickRecord);
    // Restoring is not choosing: the saved tab is left alone.
    tabs.flushPendingTabSave();
    CHECK(savedTab(store) == fv::Json(2));
}
