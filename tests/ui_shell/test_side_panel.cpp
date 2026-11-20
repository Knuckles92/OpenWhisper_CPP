// The context side panel's host on the main window (main_window.py toggle_history,
// _on_sidebar_width_animated, the panel name and refresh in _on_tab_changed), with stand-ins
// for M1.15's History sidebar and edge tab.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QLayout>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>

#include "settings/keys.h"
#include "shell_fixture.h"
#include "stand_ins.h"
#include "ui_shell/main_window.h"
#include "ui_shell/side_panel_host.h"
#include "ui_widgets/tabbed_content.h"
#include "uimath/window_geometry.h"

using fv::MainWindow;
using fv::TabbedContentWidget;
using fvtest::settleWindow;
using fvtest::ShellFixture;
using fvtest::StandInEdgeTab;
using fvtest::StandInPanel;

namespace {

// The window with the panel wired the way its feature wires it.
struct PanelWindow {
    explicit PanelWindow(ShellFixture& shell) : window(shell.window()) {
        panel = new StandInPanel;
        edgeTab = new StandInEdgeTab;
        window->setSidePanel(panel, edgeTab);
        QObject::connect(panel, &StandInPanel::widthAnimated, window.get(),
                         &MainWindow::sidePanelWidthAnimated);
        QObject::connect(window.get(), &MainWindow::sidePanelRefreshDue, panel,
                         &StandInPanel::refresh);
        QObject::connect(window.get(), &MainWindow::sidePanelPageRequested, panel,
                         &StandInPanel::showPageFor);
    }

    std::unique_ptr<MainWindow> window;
    StandInPanel* panel;
    StandInEdgeTab* edgeTab;
};

} // namespace

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_history_toggle_preserves_height
TEST_CASE("history toggle preserves height", "[test_main_window_compact][widget]") {
    ShellFixture shell;
    PanelWindow host(shell);
    MainWindow& window = *host.window;
    window.availableScreenRect = [] {
        return QRect(0, 0, 1280, 800);
    };
    window.setGeometry(40, 40, 605, 580);
    const int before = window.height();
    const int base = window.width();

    window.toggleSidePanel();
    window.sidePanelWidthAnimated(StandInPanel::kExpandedWidth);

    CHECK(window.height() == before);
    CHECK(window.width() == base + StandInPanel::kExpandedWidth);
}

TEST_CASE("the side panel widens the window by its animated width and gives it back", "[widget]") {
    ShellFixture shell;
    PanelWindow host(shell);
    MainWindow& window = *host.window;
    window.show();
    settleWindow(window);
    const QRect before = window.geometry();
    REQUIRE_FALSE(window.isSidePanelExpanded());

    window.toggleSidePanel();
    CHECK(host.edgeTab->expanded());
    // Every frame moves the window with the panel: the page keeps its width throughout.
    QElapsedTimer timer;
    timer.start();
    while (host.panel->widthAnimation()->state() == QAbstractAnimation::Running &&
           timer.elapsed() < 2000) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        CHECK(window.width() - host.panel->sidebarWidth() == before.width());
    }
    settleWindow(window);
    CHECK(window.isSidePanelExpanded());
    CHECK(window.width() == before.width() + StandInPanel::kExpandedWidth);
    CHECK(window.height() == before.height());
    CHECK(window.x() == before.x());
    CHECK(window.maximumWidth() == fv::window_size::kMaxWidth + StandInPanel::kExpandedWidth);

    window.toggleSidePanel();
    CHECK_FALSE(host.edgeTab->expanded());
    settleWindow(window);
    CHECK_FALSE(window.isSidePanelExpanded());
    CHECK(window.geometry() == before);
    CHECK(window.maximumWidth() == fv::window_size::kMaxWidth);
}

TEST_CASE("a panel the screen's edge cuts short does not shrink the page as it closes",
          "[widget]") {
    ShellFixture shell;
    PanelWindow host(shell);
    MainWindow& window = *host.window;
    window.availableScreenRect = [] {
        return QRect(0, 0, 1000, 800);
    };
    window.setGeometry(0, 0, 800, 700);

    window.toggleSidePanel();
    window.sidePanelWidthAnimated(StandInPanel::kExpandedWidth);
    // The screen holds the window to 1000 px: the page gives up 180 px while the panel is open.
    CHECK(window.width() == 1000);
    window.sidePanel()->setFixedWidth(StandInPanel::kExpandedWidth);

    window.toggleSidePanel();
    window.sidePanelWidthAnimated(0);
    CHECK(window.width() == 800);
}

TEST_CASE("the panel and its edge tab sit beside the tabs", "[widget]") {
    ShellFixture shell;
    PanelWindow host(shell);
    QLayout* content = host.window->centralWidget()->layout()->itemAt(1)->widget()->layout();
    REQUIRE(content->count() == 3);
    CHECK(content->itemAt(1)->widget() == host.edgeTab);
    CHECK(content->itemAt(2)->widget() == host.panel);
    CHECK(host.window->sidePanel() == host.panel);
    CHECK(host.window->sidePanelEdgeTab() == host.edgeTab);
    CHECK(host.edgeTab->shortcutHint() == QStringLiteral("Ctrl+H"));
    CHECK(host.edgeTab->panelName() == QStringLiteral("History"));
}

TEST_CASE("a side panel must implement the host's interface, and only one is hosted", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    QWidget plain;
    CHECK_THROWS_AS(window->setSidePanel(&plain, nullptr), std::invalid_argument);
    auto* panel = new StandInPanel;
    CHECK_THROWS_AS(window->setSidePanel(panel, &plain), std::invalid_argument);
    window->setSidePanel(panel, nullptr);
    CHECK(window->sidePanel() == panel);
    CHECK_THROWS_AS(window->setSidePanel(new StandInPanel(window.get()), nullptr),
                    std::logic_error);
}

TEST_CASE("the edge tab and the View menu item take the panel's name for the tab", "[widget]") {
    ShellFixture shell;
    PanelWindow host(shell);
    QAction* item = shell.context().menus().action(fv::menu_id::sidePanel);
    REQUIRE(item != nullptr);
    const struct {
        int tab;
        QString name;
    } cases[] = {{TabbedContentWidget::TabWalkthrough, QStringLiteral("Walkthroughs")},
                 {TabbedContentWidget::TabMeetingMode, QStringLiteral("Past Meetings")},
                 {TabbedContentWidget::TabUploadFile, QStringLiteral("History")},
                 {TabbedContentWidget::TabQuickRecord, QStringLiteral("History")}};
    for (const auto& [tab, name] : cases) {
        host.window->tabbedContent()->setCurrentIndex(tab);
        CHECK(host.edgeTab->panelName() == name);
        CHECK(item->text() == name);
        REQUIRE_FALSE(host.panel->pagesRequested.empty());
        CHECK(host.panel->pagesRequested.back() == tab);
    }
}

TEST_CASE("the panel refreshes 75 ms after a tab switch, and only while open", "[widget]") {
    ShellFixture shell;
    PanelWindow host(shell);
    MainWindow& window = *host.window;
    QSignalSpy due(&window, &MainWindow::sidePanelRefreshDue);

    window.tabbedContent()->setCurrentIndex(TabbedContentWidget::TabUploadFile);
    QTest::qWait(200);
    CHECK(due.isEmpty());

    window.toggleSidePanel();
    settleWindow(window);
    REQUIRE(window.isSidePanelExpanded());
    window.tabbedContent()->setCurrentIndex(TabbedContentWidget::TabQuickRecord);
    window.tabbedContent()->setCurrentIndex(TabbedContentWidget::TabUploadFile);
    CHECK(due.isEmpty());
    CHECK(due.wait(1000));
    // The burst of switches refreshed once.
    QTest::qWait(150);
    CHECK(due.count() == 1);
    CHECK(host.panel->refreshes == 1);

    // Closed before the refresh is due: nothing refreshes.
    window.tabbedContent()->setCurrentIndex(TabbedContentWidget::TabQuickRecord);
    window.toggleSidePanel();
    QTest::qWait(200);
    CHECK(due.count() == 1);
}

TEST_CASE("Ctrl+H and the edge tab toggle the panel", "[widget]") {
    ShellFixture shell;
    PanelWindow host(shell);
    QAction* item = shell.context().menus().action(fv::menu_id::sidePanel);
    REQUIRE(item != nullptr);
    CHECK(item->shortcut().toString() == QStringLiteral("Ctrl+H"));

    item->trigger();
    CHECK(host.window->isSidePanelExpanded());
    host.edgeTab->click();
    CHECK_FALSE(host.window->isSidePanelExpanded());
}

TEST_CASE("opening the panel from compact mode brings the full window back", "[widget]") {
    ShellFixture shell;
    PanelWindow host(shell);
    MainWindow& window = *host.window;
    window.setCompactMode(true);
    CHECK_FALSE(host.panel->isVisibleTo(&window));

    window.toggleSidePanel();
    CHECK_FALSE(window.isCompactMode());
    CHECK(window.isSidePanelExpanded());
    CHECK(host.panel->isVisibleTo(&window));

    // Already open: leaving compact mode is all the toggle does.
    window.setCompactMode(true);
    window.toggleSidePanel();
    CHECK_FALSE(window.isCompactMode());
    CHECK(window.isSidePanelExpanded());
}

TEST_CASE("the saved geometry leaves the open panel's width out", "[widget]") {
    ShellFixture shell;
    PanelWindow host(shell);
    MainWindow& window = *host.window;
    window.show();
    settleWindow(window);
    const int width = window.width();
    window.toggleSidePanel();
    settleWindow(window);
    REQUIRE(window.width() == width + StandInPanel::kExpandedWidth);

    window.saveGeometryNow();
    const fv::Json saved = shell.settings().get(fv::keys::window_geometry);
    CHECK(saved["width"] == width);
    CHECK(saved["history_expanded"] == true);

    // Restoring with the panel open adds it back, as far as the screen the saved rectangle is
    // on allows (restoring takes the real screens).
    window.resize(width, window.height());
    window.restoreWindowGeometry();
    const int screen = QGuiApplication::primaryScreen()->availableGeometry().width();
    REQUIRE(screen > width);
    CHECK(window.width() == std::min(width + StandInPanel::kExpandedWidth, screen));
}
