// The main window's Host Mode (test_host_mode.py, "the main window's view" and the controller's
// switch_to_tab): the dashboard in place of the tabs and the side panel, its own geometry,
// compact mode left and kept shut, the saved mode coming back at launch, and a tab the app opens
// leaving it.
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <QAction>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QWidget>

#include "settings/keys.h"
#include "shell_fixture.h"
#include "stand_ins.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_shell/title_bar.h"
#include "ui_widgets/tabbed_content.h"
#include "uimath/window_geometry.h"

using fv::MainWindow;
using fv::TabbedContentWidget;
using fvtest::settle;
using fvtest::ShellFixture;
using fvtest::StandInEdgeTab;
using fvtest::StandInPanel;

namespace {

// The window as test_host_mode.py's fixture has it: shown, with a side panel and its edge tab,
// and a plain dashboard.
struct HostWindow {
    explicit HostWindow(ShellFixture& shell) : window(shell.window()) {
        panel = new StandInPanel;
        edgeTab = new StandInEdgeTab;
        window->setSidePanel(panel, edgeTab);
        QObject::connect(panel, &StandInPanel::widthAnimated, window.get(),
                         &MainWindow::sidePanelWidthAnimated);
        window->setHostDashboardFactory([this](QWidget* parent) {
            dashboard = new QWidget(parent);
            return dashboard.data();
        });
        window->show();
        settle();
    }

    std::unique_ptr<MainWindow> window;
    StandInPanel* panel;
    StandInEdgeTab* edgeTab;
    QPointer<QWidget> dashboard;
};

} // namespace

TEST_CASE("test_host_mode_swaps_the_tabs_for_the_dashboard_and_back", "[widget]") {
    ShellFixture shell;
    HostWindow host(shell);
    MainWindow& window = *host.window;
    window.setGeometry(40, 50, 680, 560);
    settle();
    const QRect normal = window.geometry();
    auto& menus = shell.context().menus();

    window.setHostMode(true);
    CHECK(window.isHostMode());
    REQUIRE(host.dashboard);
    CHECK(host.dashboard->isVisibleTo(&window));
    CHECK_FALSE(window.tabbedContent()->isVisibleTo(&window));
    CHECK_FALSE(host.panel->isVisibleTo(&window));
    CHECK_FALSE(host.edgeTab->isVisibleTo(&window));
    CHECK(window.titleBar()->titleLabel()->text() == QStringLiteral("FioriVoce Host"));
    CHECK(menus.action(fv::menu_id::hostMode)->isChecked());
    CHECK_FALSE(menus.action(fv::menu_id::sidePanel)->isEnabled());
    CHECK_FALSE(menus.action(fv::menu_id::compactMode)->isEnabled());
    CHECK(shell.settings().get(fv::keys::host_mode));
    CHECK(window.width() > normal.width());

    window.setHostMode(false);
    CHECK_FALSE(window.isHostMode());
    CHECK(window.tabbedContent()->isVisibleTo(&window));
    CHECK(host.edgeTab->isVisibleTo(&window));
    CHECK_FALSE(host.dashboard->isVisibleTo(&window));
    CHECK(window.geometry() == normal);
    CHECK(window.titleBar()->titleLabel()->text() == QStringLiteral("FioriVoce"));
    CHECK(menus.action(fv::menu_id::sidePanel)->isEnabled());
    CHECK(menus.action(fv::menu_id::compactMode)->isEnabled());
    CHECK_FALSE(menus.action(fv::menu_id::hostMode)->isChecked());
    CHECK_FALSE(shell.settings().get(fv::keys::host_mode));
}

TEST_CASE("test_host_mode_keeps_its_own_window_size_and_place", "[widget]") {
    ShellFixture shell;
    HostWindow host(shell);
    MainWindow& window = *host.window;
    window.setHostMode(true);
    window.setGeometry(10, 5, 700, fv::window_size::kMinHeight);
    settle();
    window.setHostMode(false);
    CHECK(shell.settings().get(fv::keys::host_window_geometry) ==
          fv::Json{{"x", 10}, {"y", 5}, {"width", 700}, {"height", fv::window_size::kMinHeight}});
    window.setHostMode(true);
    CHECK(window.geometry() == QRect(10, 5, 700, fv::window_size::kMinHeight));
}

TEST_CASE("test_host_mode_leaves_compact_mode_and_the_recording_views_stay_shut", "[widget]") {
    ShellFixture shell;
    HostWindow host(shell);
    MainWindow& window = *host.window;
    window.setCompactMode(true);
    REQUIRE(window.isCompactMode());
    window.setHostMode(true);
    CHECK_FALSE(window.isCompactMode());
    CHECK(window.isHostMode());
    CHECK_FALSE(shell.settings().get(fv::keys::compact_mode));

    window.toggleSidePanel();
    window.toggleCompactMode();
    window.setCompactMode(true);
    CHECK_FALSE(window.isSidePanelExpanded());
    CHECK_FALSE(window.isCompactMode());
    CHECK(window.isHostMode());
}

TEST_CASE("test_saved_host_mode_comes_back_at_launch", "[widget]") {
    ShellFixture shell(fv::Json{{"host_mode", true}});
    auto window = shell.window();
    QPointer<QWidget> dashboard;
    window->setHostDashboardFactory([&](QWidget* parent) {
        dashboard = new QWidget(parent);
        return dashboard.data();
    });
    window->completeSetup();
    window->show();
    settle();
    CHECK(window->isHostMode());
    CHECK(shell.context().menus().action(fv::menu_id::hostMode)->isChecked());
    REQUIRE(dashboard);
    CHECK(dashboard->isVisibleTo(window.get()));
}

TEST_CASE("test_a_dictation_finishing_behind_the_dashboard_leaves_its_size_alone", "[widget]") {
    ShellFixture shell;
    HostWindow host(shell);
    MainWindow& window = *host.window;
    window.setHostMode(true);
    settle();
    const QSize size = window.size();
    QWidget stats(window.tabbedContent());
    stats.show();
    window.onStatsVisibilityChanged(true, &stats);
    settle();
    CHECK(window.size() == size);
}

TEST_CASE("test_a_meeting_starting_here_leaves_host_mode_for_its_tab", "[widget]") {
    ShellFixture shell;
    HostWindow host(shell);
    MainWindow& window = *host.window;
    window.setHostMode(true);
    REQUIRE(window.isHostMode());
    window.switchToTab(TabbedContentWidget::TabMeetingMode);
    CHECK_FALSE(window.isHostMode());
    CHECK(window.tabbedContent()->currentIndex() == TabbedContentWidget::TabMeetingMode);
    CHECK(window.tabbedContent()->isVisibleTo(&window));
}
