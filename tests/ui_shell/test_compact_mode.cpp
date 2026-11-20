// Compact Mode on the main window (main_window.py:1496-1625), with a stand-in for M1.9's
// CompactRecordController. Ported from tests/test_main_window_compact.py.
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QSignalSpy>

#include "settings/keys.h"
#include "shell_fixture.h"
#include "stand_ins.h"
#include "ui_shell/footer_bar.h"
#include "ui_shell/main_window.h"
#include "ui_shell/title_bar.h"
#include "ui_widgets/tabbed_content.h"
#include "ui_widgets/update_notice.h"
#include "uimath/window_fit.h"

using fv::MainWindow;
using fv::TabbedContentWidget;
using fvtest::ShellFixture;

namespace {

// The window with what Compact Mode hides and shows: the controller and the side panel.
struct CompactWindow {
    explicit CompactWindow(ShellFixture& shell) : window(shell.window()) {
        controller = new fvtest::StandInController;
        window->setCompactController(controller);
        edgeTab = new fvtest::StandInEdgeTab;
        panel = new fvtest::StandInPanel;
        window->setSidePanel(panel, edgeTab);
    }

    std::unique_ptr<MainWindow> window;
    fvtest::StandInController* controller;
    fvtest::StandInEdgeTab* edgeTab;
    fvtest::StandInPanel* panel;
};

} // namespace

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_compact_mode_uses_fixed_controller_and_restores_geometry
TEST_CASE("compact mode uses fixed controller and restores geometry",
          "[test_main_window_compact][widget]") {
    ShellFixture shell;
    CompactWindow compact(shell);
    MainWindow& window = *compact.window;
    window.setGeometry(40, 50, 620, 600);
    const QRect fullGeometry = window.geometry();

    window.setCompactMode(true);

    CHECK(window.isCompactMode());
    CHECK(window.size().width() == fv::window_size::kCompactWidth);
    CHECK(window.size().height() == fv::window_size::kCompactHeight);
    CHECK(window.minimumSizeHint().width() <= fv::window_size::kCompactWidth);
    CHECK(window.minimumSizeHint().height() <= fv::window_size::kCompactHeight);
    CHECK(compact.controller->isVisibleTo(&window));
    CHECK_FALSE(window.tabbedContent()->isVisibleTo(&window));
    CHECK_FALSE(compact.edgeTab->isVisibleTo(&window));
    CHECK_FALSE(window.footer()->statsButton()->isVisibleTo(&window));
    CHECK(window.footer()->settingsButton()->text() == QStringLiteral("Settings"));

    window.setCompactMode(false);

    CHECK_FALSE(window.isCompactMode());
    CHECK(window.geometry() == fullGeometry);
    CHECK(window.tabbedContent()->isVisibleTo(&window));
    CHECK(window.footer()->statsButton()->isVisibleTo(&window));
    CHECK(window.footer()->settingsButton()->text() == QStringLiteral("Settings"));
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_compact_mode_selection_is_persisted
TEST_CASE("compact mode selection is persisted", "[test_main_window_compact][widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->setCompactMode(true);
    CHECK(shell.settings().get(fv::keys::compact_mode));

    window->setCompactMode(false);
    CHECK_FALSE(shell.settings().get(fv::keys::compact_mode));
    CHECK(shell.settings().contents().contains("compact_mode"));
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_persisted_compact_mode_is_restored
TEST_CASE("persisted compact mode is restored", "[test_main_window_compact][widget]") {
    ShellFixture shell;
    auto window = shell.window();
    shell.settings().save_setting(fv::keys::compact_mode.key(), true);

    window->restoreCompactMode();

    CHECK(window->isCompactMode());
    CHECK(window->footer()->settingsButton()->text() == QStringLiteral("Settings"));
}

TEST_CASE("a saved compact mode is restored as the window is built", "[widget]") {
    ShellFixture shell(fv::Json{{"compact_mode", true}});
    auto window = shell.window();
    CHECK(window->isCompactMode());
    CHECK(window->size() == QSize(fv::window_size::kCompactWidth, fv::window_size::kCompactHeight));
}

TEST_CASE("a window built for features waits for completeSetup to restore compact mode",
          "[widget]") {
    ShellFixture shell(fv::Json{{"compact_mode", true}});
    MainWindow window(shell.context(), MainWindow::DeferSetup{});
    // The controller arrives while the features install, before the mode is restored.
    auto* controller = new fvtest::StandInController;
    window.setCompactController(controller);
    CHECK_FALSE(window.isCompactMode());
    window.completeSetup();
    CHECK(window.isCompactMode());
    CHECK(controller->isVisibleTo(&window));
}

TEST_CASE("compact mode hides the chrome the compact width has no room for", "[widget]") {
    ShellFixture shell;
    CompactWindow compact(shell);
    MainWindow& window = *compact.window;
    window.setUpdateNotice(QStringLiteral("2.9.0"));
    const QString fullNotice = window.updateNotice()->text();

    window.setCompactMode(true);
    CHECK_FALSE(compact.panel->isVisibleTo(&window));
    CHECK_FALSE(window.titleBar()->titleLabel()->isVisibleTo(&window));
    CHECK_FALSE(window.titleBar()->maximizeButton()->isVisibleTo(&window));
    CHECK(window.updateNotice()->text() != fullNotice);
    // The maximize button does nothing in Compact Mode, and edges do not resize.
    window.titleBar()->toggleMaximize();
    CHECK_FALSE(window.titleBar()->isMaximized());
    CHECK(window.resizeEdge(QPoint(0, 0)).none());

    window.setCompactMode(false);
    CHECK(compact.panel->isVisibleTo(&window));
    CHECK(compact.edgeTab->isVisibleTo(&window));
    CHECK(window.titleBar()->titleLabel()->isVisibleTo(&window));
    CHECK(window.titleBar()->maximizeButton()->isVisibleTo(&window));
    CHECK_FALSE(compact.controller->isVisibleTo(&window));
    CHECK(window.updateNotice()->text() == fullNotice);
    CHECK(window.minimumWidth() == fv::window_size::kMinWidth);
    CHECK(window.maximumWidth() == fv::window_size::kMaxWidth);
}

TEST_CASE("the compact window keeps its own position", "[widget]") {
    ShellFixture shell;
    CompactWindow compact(shell);
    MainWindow& window = *compact.window;
    window.setGeometry(60, 40, 680, 680);

    window.setCompactMode(true);
    CHECK(window.pos() == QPoint(60, 40));
    window.move(100, 120);
    window.setCompactMode(false);
    CHECK(window.pos() == QPoint(60, 40));
    const fv::Json saved = shell.settings().get(fv::keys::compact_window_geometry);
    CHECK(saved == fv::Json{{"x", 100}, {"y", 120}});
    // The full window's own geometry was saved as compact mode began.
    CHECK(shell.settings().get(fv::keys::window_geometry)["x"] == 60);

    window.setCompactMode(true);
    CHECK(window.pos() == QPoint(100, 120));
}

TEST_CASE("an unreadable compact position keeps the window where it is", "[widget]") {
    ShellFixture shell(fv::Json{{"compact_window_geometry", {{"x", "left"}, {"y", 5}}}});
    auto window = shell.window();
    window->setGeometry(70, 80, 680, 680);
    window->setCompactMode(true);
    CHECK(window->pos() == QPoint(70, 80));
}

TEST_CASE("the View menu's Compact Mode toggles it", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    QSignalSpy changed(window.get(), &MainWindow::compactModeChanged);
    QAction* action = shell.context().menus().action(fv::menu_id::compactMode);
    REQUIRE(action != nullptr);

    action->trigger();
    CHECK(window->isCompactMode());
    action->trigger();
    CHECK_FALSE(window->isCompactMode());
    REQUIRE(changed.count() == 2);
    CHECK(changed.at(0).at(0).toBool());
    CHECK_FALSE(changed.at(1).at(0).toBool());

    // Asking for the mode it is in changes nothing.
    window->setCompactMode(false);
    CHECK(changed.count() == 2);
}

TEST_CASE("switching to another tab leaves compact mode", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->setCompactMode(true);
    window->tabbedContent()->setCurrentIndex(TabbedContentWidget::TabQuickRecord);
    CHECK(window->isCompactMode());
    window->tabbedContent()->setCurrentIndex(TabbedContentWidget::TabUploadFile);
    CHECK_FALSE(window->isCompactMode());
    CHECK_FALSE(shell.settings().get(fv::keys::compact_mode));
}

TEST_CASE("a compact mode that cannot be saved is kept", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    // A minimized window saves no geometry on the way in, so the failure meets compact_mode.
    window->setWindowState(Qt::WindowMinimized);
    shell.settings().file().fail_next_replace("disk full");
    window->setCompactMode(true);
    CHECK(window->isCompactMode());
    CHECK_FALSE(shell.settings().get(fv::keys::compact_mode));
}

TEST_CASE("a later controller replaces the first", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    auto* first = new fvtest::StandInController;
    auto* second = new fvtest::StandInController;
    window->setCompactController(first);
    window->setCompactController(second);
    window->setCompactMode(true);
    CHECK(window->compactController() == second);
    CHECK(second->isVisibleTo(window.get()));
    CHECK_FALSE(first->isVisibleTo(window.get()));

    // Passing the same controller again leaves it in the layout once.
    QLayout* layout = second->parentWidget()->layout();
    REQUIRE(layout != nullptr);
    const int items = layout->count();
    window->setCompactController(second);
    CHECK(layout->count() == items);
    CHECK(second->isVisibleTo(window.get()));
}
