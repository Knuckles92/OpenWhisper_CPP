// The main window's walkthrough tab switching (main_window.py:1154-1180), from
// tests/test_main_window_compact.py, over a stand-in Walkthrough screen.
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <QPointer>

#include "appkit/order.h"
#include "appkit/registries.h"
#include "shell_fixture.h"
#include "stand_ins.h"
#include "ui_shell/main_window.h"
#include "ui_widgets/tabbed_content.h"

using fv::TabbedContentWidget;
using fvtest::ShellFixture;
using fvtest::StandInScreen;

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_a_walkthrough_ended_in_the_tray_reopens_on_its_tab
TEST_CASE("a walkthrough ended in the tray reopens on its tab", "[widget]") {
    ShellFixture shell;
    auto made = std::make_shared<QPointer<StandInScreen>>();
    shell.context().screens().define(fv::screen_key::walkthrough, fv::order::tab::walkthrough,
                                     QStringLiteral("Stand-in"),
                                     [made](QWidget* parent) -> QWidget* {
                                         *made = new StandInScreen(parent);
                                         return made->data();
                                     });
    auto window = shell.window();
    window->setTrayAvailable(true);
    TabbedContentWidget& tabs = *window->tabbedContent();
    window->show();
    window->setRecordingActive(true);
    window->setWalkthroughActive(true);
    // "Minimize the window during walkthroughs".
    window->minimizeToTray();
    REQUIRE_FALSE(window->isVisible());

    window->setWalkthroughActive(false);
    window->setRecordingActive(false);
    window->restoreFromTray();

    CHECK(tabs.currentIndex() == TabbedContentWidget::TabWalkthrough);
}
