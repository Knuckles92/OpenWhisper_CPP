// The main window's chrome: tabs, footer, menus, tray-less desktops, geometry and edge resize.
// Ported from tests/test_main_window_compact.py and tests/test_window_fits_tab.py, plus the
// registry-driven parts OpenWhisper_CPP adds.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QMargins>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>

#include "appkit/registries.h"
#include "settings/keys.h"
#include "shell_fixture.h"
#include "ui_shell/footer_bar.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_shell/title_bar.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/tabbed_content.h"
#include "ui_widgets/update_notice.h"
#include "uimath/window_geometry.h"

using fv::MainWindow;
using fv::TabbedContentWidget;
using fvtest::settle;
using fvtest::ShellFixture;

namespace {

QStringList actionTexts(QMenu* menu) {
    QStringList texts;
    for (QAction* action : menu->actions()) {
        texts << (action->isSeparator() ? QStringLiteral("---") : action->text());
    }
    return texts;
}

QMenu* titleMenu(MainWindow& window, int index) {
    return window.titleBar()->menuBar()->actions().at(index)->menu();
}

fv::Json savedGeometry(int width, int height) {
    return fv::Json{{"x", 10},
                    {"y", 10},
                    {"width", width},
                    {"height", height},
                    {"format", std::string(fv::kCollapsedContentFormat)},
                    {"history_expanded", false}};
}

// test_window_fits_tab.py's fixture: shown on a 1920 x 1080 screen, then settled.
std::unique_ptr<MainWindow> shownWindow(ShellFixture& shell) {
    auto window = shell.window();
    window->show();
    settle();
    return window;
}

} // namespace

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_launch_restores_last_tab_and_reshow_keeps_current_tab
TEST_CASE("launch restores last tab and reshow keeps current tab",
          "[test_main_window_compact][widget]") {
    const int lastTab = GENERATE(0, 1, 2, 3);
    ShellFixture shell(fv::Json{{"last_tab_index", TabbedContentWidget::savedTabId(lastTab)}});
    auto window = shell.window();
    window->show();
    QApplication::processEvents();
    CHECK(window->tabbedContent()->currentIndex() == lastTab);
    CHECK(window->tabbedContent()->stack()->currentIndex() == lastTab);

    window->tabbedContent()->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
    window->hide();
    window->show();
    QApplication::processEvents();
    CHECK(window->tabbedContent()->currentIndex() == TabbedContentWidget::TabMeetingMode);
    CHECK(window->tabbedContent()->stack()->currentWidget() ==
          window->page(TabbedContentWidget::TabMeetingMode));
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_missing_system_tray_disables_tray_only_controls
TEST_CASE("missing system tray disables tray only controls", "[test_main_window_compact][widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->setTrayAvailable(false);

    CHECK(window->footer()->trayButton()->isHidden());
    // main_window.py set_tray_available's tooltip on the disabled button.
    CHECK_FALSE(window->footer()->trayButton()->isEnabled());
    CHECK(window->footer()->trayButton()->toolTip() ==
          QStringLiteral("This desktop session does not provide a system tray."));
    QAction* minimize = shell.context().menus().action(fv::menu_id::minimizeToTray);
    REQUIRE(minimize != nullptr);
    CHECK_FALSE(minimize->isVisible());

    window->minimizeToTray();
    QApplication::processEvents();
    CHECK(window->isMinimized());
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_close_is_not_hidden_when_system_tray_is_unavailable
TEST_CASE("close is not hidden when system tray is unavailable",
          "[test_main_window_compact][widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->setTrayAvailable(false);
    QCloseEvent event;
    QApplication::sendEvent(window.get(), &event);
    CHECK(event.isAccepted());
}

TEST_CASE("close hides the window to the tray while minimize_tray is on", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->show();
    QApplication::processEvents();
    QCloseEvent event;
    QApplication::sendEvent(window.get(), &event);
    CHECK_FALSE(event.isAccepted());
    CHECK_FALSE(window->isVisible());

    shell.settings().save_setting(fv::keys::minimize_tray.key(), false);
    window->show();
    QCloseEvent second;
    QApplication::sendEvent(window.get(), &second);
    CHECK(second.isAccepted());
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_footer_has_one_settings_entry_point
TEST_CASE("footer has one settings entry point", "[test_main_window_compact][widget]") {
    ShellFixture shell;
    auto window = shell.window();
    QStringList labels;
    for (QPushButton* button : window->footer()->findChildren<QPushButton*>()) {
        labels << button->text();
    }
    CHECK_FALSE(labels.contains(QStringLiteral("Model Manager")));
    CHECK(labels.contains(QStringLiteral("Settings")));
    CHECK(labels == QStringList{QStringLiteral("Minimize to Tray"), QStringLiteral("Stats"),
                                QStringLiteral("Settings"), QStringLiteral("Quit")});
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_footer_settings_button_opens_settings
TEST_CASE("footer settings button opens settings", "[test_main_window_compact][widget]") {
    ShellFixture shell;
    auto window = shell.window();
    QSignalSpy opened(window.get(), &MainWindow::settingsRequested);
    window->footer()->settingsButton()->click();
    CHECK(opened.count() == 1);
    CHECK(window->footer()->settingsButton()->text() == QStringLiteral("Settings"));
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_footer_stats_button_opens_stats
TEST_CASE("footer stats button opens stats", "[test_main_window_compact][widget]") {
    ShellFixture shell;
    auto window = shell.window();
    QSignalSpy opened(window.get(), &MainWindow::statsRequested);
    window->footer()->statsButton()->click();
    CHECK(opened.count() == 1);
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_file_menu_downloads_opens_downloads
TEST_CASE("file menu downloads opens downloads", "[test_main_window_compact][widget]") {
    ShellFixture shell;
    auto window = shell.window();
    QSignalSpy opened(window.get(), &MainWindow::settingsDestinationRequested);
    QMenu* file = titleMenu(*window, 0);
    REQUIRE(file != nullptr);
    CHECK_FALSE(actionTexts(file).contains(QStringLiteral("Model Manager...")));

    QAction* downloads = nullptr;
    for (QAction* action : file->actions()) {
        if (action->text() == QStringLiteral("Downloads...")) {
            downloads = action;
        }
    }
    REQUIRE(downloads != nullptr);
    downloads->trigger();
    REQUIRE(opened.count() == 1);
    CHECK(opened.at(0).at(0).toString() == QStringLiteral("downloads"));
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_restore_clamps_short_saved_geometry_to_scroll_free_height
TEST_CASE("restore clamps short saved geometry to scroll free height",
          "[test_main_window_compact][widget]") {
    ShellFixture shell;
    auto window = shell.window();
    shell.settings().save_setting(fv::keys::window_geometry.key(),
                                  savedGeometry(fv::window_size::kDefaultWidth, 500));
    window->restoreWindowGeometry();
    CHECK(window->height() == fv::window_size::kMinHeight);
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_restore_clamps_narrow_saved_geometry_to_full_ui_width
TEST_CASE("restore clamps narrow saved geometry to full ui width",
          "[test_main_window_compact][widget]") {
    ShellFixture shell;
    auto window = shell.window();
    shell.settings().save_setting(fv::keys::window_geometry.key(),
                                  savedGeometry(500, fv::window_size::kDefaultHeight));
    window->restoreWindowGeometry();
    CHECK(window->width() == fv::window_size::kDefaultWidth);
}

TEST_CASE("a saved geometry that is not whole numbers centres the window", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    fv::Json geometry = savedGeometry(700, 680);
    geometry["width"] = 700.5;
    shell.settings().save_setting(fv::keys::window_geometry.key(), geometry);
    window->restoreWindowGeometry();
    CHECK(window->width() == fv::window_size::kDefaultWidth);
    const QRect screen = window->availableScreenRect();
    CHECK(window->x() == screen.x() + (screen.width() - window->width()) / 2);
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_minimum_full_width_keeps_tabs_and_footer_controls_separate
TEST_CASE("minimum full width keeps tabs and footer controls separate",
          "[test_main_window_compact][widget]") {
    fvtest::AppTheme theme;
    ShellFixture shell;
    auto window = shell.window();
    window->resize(fv::window_size::kMinWidth, fv::window_size::kDefaultHeight);
    window->show();
    QApplication::processEvents();

    QTabBar* tabBar = window->tabbedContent()->tabBar();
    const QRect lastTab = tabBar->tabRect(tabBar->count() - 1);
    CHECK(lastTab.right() < tabBar->width());

    const std::vector<QWidget*> buttons{
        window->footer()->trayButton(), window->footer()->statsButton(),
        window->footer()->settingsButton(), window->footer()->quitButton()};
    for (std::size_t i = 0; i + 1 < buttons.size(); ++i) {
        CHECK(buttons[i]->geometry().right() < buttons[i + 1]->geometry().left());
    }
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_clamp_keeps_footer_on_short_screen
TEST_CASE("clamp keeps footer on short screen", "[test_main_window_compact][widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->availableScreenRect = [] {
        return QRect(0, 0, 1280, 800);
    };
    const QRect clamped = window->clampGeometry(0, 0, fv::window_size::kDefaultWidth, 840);
    CHECK(clamped.height() == 800);
    CHECK(clamped.width() == fv::window_size::kDefaultWidth);
    CHECK(clamped.x() >= 0);
    CHECK(clamped.y() >= 0);
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_geometry_save_waits_until_initial_show
TEST_CASE("geometry save waits until initial show", "[test_main_window_compact][widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->setInitialShowComplete(false);
    window->scheduleGeometrySave();
    CHECK_FALSE(window->geometrySaveScheduled());
}

TEST_CASE("a moved window saves its geometry half a second later", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->show();
    QApplication::processEvents();
    window->move(window->x() + 10, window->y() + 10);
    QApplication::processEvents();
    CHECK(window->geometrySaveScheduled());
    REQUIRE(fvtest::waitUntil(
        [&] { return !shell.settings().get(fv::keys::window_geometry).is_null(); }));
    const fv::Json saved = shell.settings().get(fv::keys::window_geometry);
    CHECK(saved["x"] == window->x());
    CHECK(saved["y"] == window->y());
    CHECK(saved["width"] == window->width());
    CHECK(saved["height"] == window->height());
    CHECK(saved["format"] == "collapsed_content_v1");
    CHECK(saved["history_expanded"] == false);
    std::vector<std::string> keys;
    for (const auto& [key, value] : saved.items()) {
        keys.push_back(key);
    }
    CHECK(keys ==
          std::vector<std::string>{"x", "y", "width", "height", "format", "history_expanded"});
}

// from tests/test_window_fits_tab.py::test_dragging_an_edge_of_a_maximized_window_leaves_maximized
TEST_CASE("dragging an edge of a maximized window leaves maximized",
          "[test_window_fits_tab][widget]") {
    ShellFixture shell;
    auto window = shownWindow(shell);
    window->titleBar()->toggleMaximize();
    settle();
    REQUIRE(window->isMaximized());

    const QRect geometry = window->geometry();
    const QPoint start = geometry.topRight();
    window->beginResize(fv::ResizeEdge{1, 0}, start);
    CHECK_FALSE(window->isMaximized());
    CHECK_FALSE(window->titleBar()->isMaximized());
    CHECK(window->titleBar()->maximizeButton()->glyphName() == QStringLiteral("maximize"));
    CHECK(window->geometry() == geometry);

    window->applyResizeDelta(start - QPoint(100, 0));
    window->finishResize();
    CHECK(window->geometry().topLeft() == geometry.topLeft());
    CHECK(window->width() == std::max(window->minimumWidth(), geometry.width() - 100));
}

// from tests/test_window_fits_tab.py::test_restoring_from_maximized_leaves_the_maximized_state
TEST_CASE("restoring from maximized leaves the maximized state", "[test_window_fits_tab][widget]") {
    ShellFixture shell;
    auto window = shownWindow(shell);
    const QRect normal = window->geometry();
    window->titleBar()->toggleMaximize();
    settle();
    window->titleBar()->toggleMaximize();
    settle();

    CHECK_FALSE(window->isMaximized());
    CHECK(window->geometry().topLeft() == normal.topLeft());
    CHECK(window->width() == normal.width());
}

// from tests/test_window_fits_tab.py::test_the_left_edge_stops_at_the_width_cap_without_sliding
TEST_CASE("the left edge stops at the width cap without sliding",
          "[test_window_fits_tab][widget]") {
    ShellFixture shell;
    auto window = shownWindow(shell);
    const int right = window->geometry().right();
    const QPoint start = window->geometry().topLeft();
    window->beginResize(fv::ResizeEdge{-1, 0}, start);
    window->applyResizeDelta(start - QPoint(5000, 0));
    window->finishResize();

    CHECK(window->width() == window->maximumWidth());
    CHECK(window->geometry().right() == right);
}

TEST_CASE("an edge within eight pixels resizes and the middle does not", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->resize(680, 680);
    CHECK(window->resizeEdge(QPoint(8, 300)) == fv::ResizeEdge{-1, 0});
    CHECK(window->resizeEdge(QPoint(9, 300)) == fv::ResizeEdge{0, 0});
    CHECK(window->resizeEdge(QPoint(672, 672)) == fv::ResizeEdge{1, 1});
    CHECK(window->resizeEdge(QPoint(300, 0)) == fv::ResizeEdge{0, -1});
    CHECK(window->resizeEdge(QPoint(340, 340)).none());
}

TEST_CASE("the title bar menus hold OpenWhisper's items in order", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    QMenuBar* bar = window->titleBar()->menuBar();
    QStringList menus;
    for (QAction* action : bar->actions()) {
        menus << action->text();
    }
    CHECK(menus ==
          QStringList{QStringLiteral("File"), QStringLiteral("View"), QStringLiteral("Help")});
    CHECK(actionTexts(titleMenu(*window, 0)) ==
          QStringList{QStringLiteral("Settings"), QStringLiteral("Guided setup…"),
                      QStringLiteral("Downloads..."), QStringLiteral("Hotkeys"),
                      QStringLiteral("---"), QStringLiteral("Minimize to Tray"),
                      QStringLiteral("Exit")});
    CHECK(actionTexts(titleMenu(*window, 1)) ==
          QStringList{QStringLiteral("History"), QStringLiteral("History Calendar"),
                      QStringLiteral("Compact Mode"), QStringLiteral("Host Mode"),
                      QStringLiteral("---"), QStringLiteral("Scratchpad"), QStringLiteral("Stats"),
                      QStringLiteral("---"), QStringLiteral("Open Meeting Dashboard")});
    CHECK(actionTexts(titleMenu(*window, 2)) ==
          QStringList{QStringLiteral("Check for Updates..."), QStringLiteral("About")});
    CHECK_FALSE(window->menuBar()->isVisible());

    fv::MenuRegistry& registry = shell.context().menus();
    const auto shortcut = [&](const char* id) {
        return registry.action(id)->shortcut().toString();
    };
    CHECK(shortcut(fv::menu_id::exit) == QStringLiteral("Ctrl+Q"));
    CHECK(shortcut(fv::menu_id::sidePanel) == QStringLiteral("Ctrl+H"));
    CHECK(shortcut(fv::menu_id::historyCalendar) == QStringLiteral("Ctrl+Shift+D"));
    CHECK(shortcut(fv::menu_id::compactMode) == QStringLiteral("Ctrl+Shift+C"));
    CHECK(shortcut(fv::menu_id::hostMode) == QStringLiteral("Ctrl+Shift+H"));
    CHECK(registry.action(fv::menu_id::hostMode)->isCheckable());
    // Under Qt's default role an item called "Settings" would read "Preferences".
    CHECK(registry.action(fv::menu_id::settings)->menuRole() == QAction::NoRole);
    CHECK(registry.action(fv::menu_id::exit)->menuRole() == QAction::NoRole);
    CHECK(registry.action(fv::menu_id::about)->menuRole() == QAction::NoRole);
}

TEST_CASE("the side panel item is named for the meeting tab", "[widget]") {
    ShellFixture shell(fv::Json{
        {"last_tab_index", TabbedContentWidget::savedTabId(TabbedContentWidget::TabMeetingMode)}});
    auto window = shell.window();
    CHECK(shell.context().menus().action(fv::menu_id::sidePanel)->text() ==
          QStringLiteral("Past Meetings"));
}

TEST_CASE("menu items reach the window's signals", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    fv::MenuRegistry& registry = shell.context().menus();
    QSignalSpy settings(window.get(), &MainWindow::settingsRequested);
    QSignalSpy setup(window.get(), &MainWindow::guidedSetupRequested);
    QSignalSpy hotkeys(window.get(), &MainWindow::hotkeysRequested);
    QSignalSpy scratchpad(window.get(), &MainWindow::scratchpadRequested);
    QSignalSpy stats(window.get(), &MainWindow::statsRequested);
    QSignalSpy dashboard(window.get(), &MainWindow::meetingDashboardRequested);
    QSignalSpy updates(window.get(), &MainWindow::checkForUpdatesRequested);
    QSignalSpy about(window.get(), &MainWindow::aboutRequested);
    for (const char* id :
         {fv::menu_id::settings, fv::menu_id::guidedSetup, fv::menu_id::hotkeys,
          fv::menu_id::scratchpad, fv::menu_id::stats, fv::menu_id::meetingDashboard,
          fv::menu_id::checkForUpdates, fv::menu_id::about}) {
        registry.action(id)->trigger();
    }
    for (const QSignalSpy* spy :
         {&settings, &setup, &hotkeys, &scratchpad, &stats, &dashboard, &updates, &about}) {
        CHECK(spy->count() == 1);
    }
}

TEST_CASE("the upload tab builds its screen on first show", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    QSignalSpy created(window.get(), &MainWindow::screenCreated);
    QWidget* placeholder = window->page(TabbedContentWidget::TabUploadFile);
    CHECK(shell.context().screens().existing(fv::ShellFeature::kUploadScreen) == nullptr);

    window->tabbedContent()->setCurrentIndex(TabbedContentWidget::TabUploadFile);
    QWidget* upload = shell.context().screens().existing(fv::ShellFeature::kUploadScreen);
    REQUIRE(upload != nullptr);
    CHECK(window->page(TabbedContentWidget::TabUploadFile) == upload);
    CHECK(window->page(TabbedContentWidget::TabUploadFile) != placeholder);
    CHECK(window->tabbedContent()->stack()->currentWidget() == upload);
    REQUIRE(created.count() == 1);
    CHECK(created.at(0).at(0).toString() == QStringLiteral("upload"));

    window->tabbedContent()->setCurrentIndex(TabbedContentWidget::TabQuickRecord);
    window->tabbedContent()->setCurrentIndex(TabbedContentWidget::TabUploadFile);
    CHECK(created.count() == 1);
}

TEST_CASE("a quit the confirmation refuses keeps the app running", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    int asked = 0;
    window->confirmQuit = [&asked] {
        ++asked;
        return false;
    };
    bool quit = false;
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, [&quit] { quit = true; });
    window->requestQuit();
    QApplication::processEvents();
    CHECK(asked == 1);
    CHECK_FALSE(quit);
}

TEST_CASE("restore from tray shows a hidden window normal", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->showMinimized();
    QApplication::processEvents();
    window->restoreFromTray();
    QApplication::processEvents();
    CHECK(window->isVisible());
    CHECK_FALSE(window->isMinimized());
    CHECK_FALSE(window->titleBar()->isMaximized());

    window->toggleTrayVisibility();
    CHECK_FALSE(window->isVisible());
    window->toggleTrayVisibility();
    CHECK(window->isVisible());
}

TEST_CASE("the window carries the product name and its sizes", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    CHECK(window->windowTitle() == QStringLiteral("OpenWhisper_CPP"));
    CHECK(window->minimumSize() == QSize(650, 660));
    CHECK(window->maximumWidth() == 1280);
    CHECK((window->windowFlags() & Qt::FramelessWindowHint));
    CHECK(window->centralWidget()->objectName() == QStringLiteral("centralWidget"));
    CHECK(window->footer()->objectName() == QStringLiteral("footerBar"));
    CHECK(window->titleBar()->objectName() == QStringLiteral("customTitleBar"));
}

// The main window has no pixel reference in M0 (its tabs differ until M1 and M2); its chrome is
// MainWindow._setup_ui's: the title bar, then the content beside where the History edge tab and
// side panel go (M1.21), then the footer.
TEST_CASE("the window's chrome is the title bar, the tabs and the footer", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    QLayout* outer = window->centralWidget()->layout();
    REQUIRE(outer != nullptr);
    REQUIRE(outer->count() == 3);
    CHECK(outer->itemAt(0)->widget() == window->titleBar());
    CHECK(outer->itemAt(2)->widget() == window->footer());
    CHECK(outer->contentsMargins() == QMargins(1, 1, 1, 1));
    CHECK(outer->spacing() == 0);

    QWidget* content = outer->itemAt(1)->widget();
    REQUIRE(content != nullptr);
    REQUIRE(content->layout()->count() == 1);
    QWidget* mainArea = content->layout()->itemAt(0)->widget();
    REQUIRE(mainArea != nullptr);
    CHECK(mainArea->layout()->itemAt(0)->widget() == window->tabbedContent());

    QTabBar* tabs = window->tabbedContent()->tabBar();
    QStringList titles;
    for (int i = 0; i < tabs->count(); ++i) {
        titles << tabs->tabText(i);
    }
    CHECK(titles == QStringList{QStringLiteral("Quick Record"), QStringLiteral("Upload File"),
                                QStringLiteral("Walkthrough"), QStringLiteral("Meeting Mode")});
    CHECK(window->tabbedContent()->stack()->count() == 4);
    // The update notice sits just before the window buttons and is hidden without a release.
    CHECK(window->updateNotice()->isHidden());
    auto* bar = static_cast<QHBoxLayout*>(window->titleBar()->layout());
    CHECK(bar->indexOf(window->updateNotice()) + 1 ==
          bar->indexOf(window->titleBar()->minimizeButton()));
}

TEST_CASE("update hotkeys gives the tray button its hint", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->updateHotkeys(QStringLiteral("*"), QStringLiteral("-"), QString(),
                          QStringLiteral("Ctrl+Alt+M"), QString(), QString());
    fv::Button* tray = window->footer()->trayButton();
    tray->hotkeyHintFilter()->showHint();
    REQUIRE(tray->hotkeyHintFilter()->hint() != nullptr);
    CHECK(tray->hotkeyHintFilter()->hint()->findChild<QLabel*>()->text() ==
          QStringLiteral("Ctrl+Alt+M"));
}

TEST_CASE("View Host Mode lazily replaces the recording tabs and keeps its own geometry",
          "[widget]") {
    ShellFixture shell;
    auto window = shownWindow(shell);
    auto* command = shell.context().menus().action(fv::menu_id::hostMode);
    auto* compact = shell.context().menus().action(fv::menu_id::compactMode);
    auto* history = shell.context().menus().action(fv::menu_id::sidePanel);
    REQUIRE(command);
    REQUIRE(compact);
    REQUIRE(history);

    int builds = 0;
    QPointer<QWidget> dashboard;
    window->setHostDashboardFactory([&](QWidget* parent) {
        ++builds;
        dashboard = new QWidget(parent);
        return dashboard.data();
    });
    CHECK(builds == 0);
    command->trigger();
    REQUIRE(dashboard);
    CHECK(builds == 1);
    CHECK(window->isHostMode());
    CHECK(command->isChecked());
    CHECK(window->tabbedContent()->isHidden());
    CHECK_FALSE(dashboard->isHidden());
    CHECK_FALSE(history->isEnabled());
    CHECK_FALSE(compact->isEnabled());
    CHECK(shell.settings().get(fv::keys::host_mode));

    command->trigger();
    CHECK_FALSE(window->isHostMode());
    CHECK_FALSE(window->tabbedContent()->isHidden());
    CHECK(dashboard->isHidden());
    CHECK(history->isEnabled());
    CHECK(compact->isEnabled());
    CHECK_FALSE(shell.settings().get(fv::keys::host_mode));
    CHECK(shell.settings().get(fv::keys::host_window_geometry).is_object());

    command->trigger();
    CHECK(builds == 1);
}

// tests/test_host_mode.py's title and first size (map row A01-53): "<product> Host" in the title
// bar, and a first Host Mode window grows from where it is to at least 780 x 780.
TEST_CASE("a first Host Mode grows the window to 780 square and names it Host", "[widget]") {
    ShellFixture shell;
    auto window = shownWindow(shell);
    window->availableScreenRect = [] {
        return QRect(0, 0, 1920, 1080);
    };
    window->setGeometry(100, 120, 700, 690);
    settle();
    window->setHostDashboardFactory([](QWidget* parent) { return new QWidget(parent); });
    auto* command = shell.context().menus().action(fv::menu_id::hostMode);
    REQUIRE(command);

    command->trigger();
    REQUIRE(window->isHostMode());
    CHECK(window->titleBar()->titleLabel()->text() == QStringLiteral("OpenWhisper_CPP Host"));
    CHECK(window->geometry() == QRect(100, 120, 780, 780));

    command->trigger();
    CHECK(window->titleBar()->titleLabel()->text() == QStringLiteral("OpenWhisper_CPP"));
}

// main_window.py _build_footer (map row A01-45): a 48 px bar, 34 px buttons with their minimum
// widths, 10 px gaps between them, centred, and each button's tooltip.
TEST_CASE("the footer's buttons keep OpenWhisper's sizes, gaps and tooltips", "[widget]") {
    fv::FooterBar footer;
    CHECK(footer.objectName() == QStringLiteral("footerBar"));
    CHECK(footer.minimumHeight() == 48);
    CHECK(footer.maximumHeight() == 48);
    auto* layout = qobject_cast<QHBoxLayout*>(footer.layout());
    REQUIRE(layout != nullptr);
    CHECK(layout->contentsMargins() == QMargins(16, 7, 16, 7));
    CHECK(layout->spacing() == 0);
    // stretch | tray | 10 | stats | 10 | settings | 10 | quit | stretch
    REQUIRE(layout->count() == 9);
    CHECK(layout->itemAt(0)->spacerItem() != nullptr);
    CHECK(layout->itemAt(8)->spacerItem() != nullptr);
    for (const int gap : {2, 4, 6}) {
        REQUIRE(layout->itemAt(gap)->spacerItem() != nullptr);
        CHECK(layout->itemAt(gap)->spacerItem()->sizeHint().width() == 10);
    }
    const std::vector<std::tuple<QPushButton*, QString, int, QString>> buttons{
        {footer.trayButton(), QStringLiteral("Minimize to Tray"), 140,
         QStringLiteral("Hide OpenWhisper_CPP in the system tray")},
        {footer.statsButton(), QStringLiteral("Stats"), 80,
         QStringLiteral("Your words, pace, streak and favorite apps")},
        {footer.settingsButton(), QStringLiteral("Settings"), 100,
         QStringLiteral("Settings, models, and downloads (starts on the Overview)")},
        {footer.quitButton(), QStringLiteral("Quit"), 100, QString()},
    };
    int index = 1;
    for (const auto& [button, text, width, tip] : buttons) {
        INFO(text.toStdString());
        CHECK(layout->itemAt(index)->widget() == button);
        index += 2;
        CHECK(button->text() == text);
        CHECK(button->minimumWidth() == width);
        CHECK(button->minimumHeight() == 34);
        CHECK(button->maximumHeight() == 34);
        CHECK(button->cursor().shape() == Qt::PointingHandCursor);
        if (!tip.isEmpty()) {
            CHECK(button->toolTip() == tip);
        }
    }
}
