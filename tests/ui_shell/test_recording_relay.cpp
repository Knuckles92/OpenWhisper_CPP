// The main window's recording-state relay and transient statuses (main_window.py:1154-1180,
// 2064-2071), and the window the shell feature provides, with stand-in screens and controller.
// Ported from tests/test_main_window_compact.py, plus the cases its lines define.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <memory>
#include <string>

#include <QApplication>
#include <QElapsedTimer>
#include <QMenuBar>
#include <QPointer>
#include <QStackedWidget>
#include <QTabBar>
#include <QTest>

#include "appkit/order.h"
#include "appkit/registries.h"
#include "shell_fixture.h"
#include "stand_ins.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_shell/title_bar.h"
#include "ui_widgets/tabbed_content.h"

using fv::MainWindow;
using fv::TabbedContentWidget;
using fvtest::ShellFixture;
using fvtest::StandInScreen;

namespace {

using ScreenRef = std::shared_ptr<QPointer<StandInScreen>>;

ScreenRef addScreen(ShellFixture& shell, const char* key, int order) {
    auto made = std::make_shared<QPointer<StandInScreen>>();
    shell.context().screens().define(key, order, QStringLiteral("Stand-in"),
                                     [made](QWidget* parent) -> QWidget* {
                                         *made = new StandInScreen(parent);
                                         return made->data();
                                     });
    return made;
}

// Quick Record as the window builds it: a page that takes the relayed calls.
std::shared_ptr<QPointer<fvtest::StandInPage>> addQuickRecord(ShellFixture& shell) {
    auto made = std::make_shared<QPointer<fvtest::StandInPage>>();
    shell.context().screens().addScreen(
        fv::ScreenSpec{fv::order::tab::quick_record, QStringLiteral("Quick Record"),
                       [made](QWidget* parent) -> QWidget* {
                           *made = new fvtest::StandInPage({120}, parent);
                           return made->data();
                       }});
    return made;
}

void clickTab(TabbedContentWidget& tabs, int index) {
    QTest::mouseClick(tabs.tabBar(), Qt::LeftButton, Qt::NoModifier,
                      tabs.tabBar()->tabRect(index).center());
}

} // namespace

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_can_switch_tabs_during_a_recording
TEST_CASE("can switch tabs during a recording", "[test_main_window_compact][widget]") {
    const bool walkthrough = GENERATE(false, true);
    ShellFixture shell;
    ScreenRef walkthroughTab =
        addScreen(shell, fv::screen_key::walkthrough, fv::order::tab::walkthrough);
    ScreenRef meetingTab = addScreen(shell, fv::screen_key::meeting, fv::order::tab::meeting_mode);
    auto window = shell.window();
    TabbedContentWidget& tabs = *window->tabbedContent();
    tabs.unlockMeetingTab();
    window->show();
    tabs.setCurrentIndex(TabbedContentWidget::TabUploadFile);
    window->setRecordingActive(true);
    CHECK(tabs.currentIndex() == TabbedContentWidget::TabQuickRecord);

    if (walkthrough) {
        window->setWalkthroughActive(true);
        CHECK(tabs.currentIndex() == TabbedContentWidget::TabWalkthrough);
    }

    for (const int index :
         {TabbedContentWidget::TabWalkthrough, TabbedContentWidget::TabQuickRecord,
          TabbedContentWidget::TabMeetingMode, TabbedContentWidget::TabUploadFile}) {
        clickTab(tabs, index);
        CHECK(tabs.currentIndex() == index);
        CHECK(tabs.stack()->currentIndex() == index);
        CHECK(window->isRecording());
    }

    // The screens built during the recording took its state as they opened.
    REQUIRE_FALSE(meetingTab->isNull());
    REQUIRE_FALSE(walkthroughTab->isNull());
    CHECK((*meetingTab)->calls.last() == QStringLiteral("setRecordingActive:1"));
    CHECK((*walkthroughTab)->calls.last() == (walkthrough
                                                  ? QStringLiteral("setRecordingState:1,1")
                                                  : QStringLiteral("setRecordingState:1,0")));

    if (walkthrough) {
        window->setWalkthroughActive(false);
    }
    window->setRecordingActive(false);
    CHECK(tabs.currentIndex() == TabbedContentWidget::TabUploadFile);
    CHECK((*meetingTab)->calls.last() == QStringLiteral("setRecordingActive:0"));
    CHECK((*walkthroughTab)->calls.last() == QStringLiteral("setRecordingState:0,0"));
}

TEST_CASE("only a recording that starts brings the window to quick record", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    TabbedContentWidget& tabs = *window->tabbedContent();
    window->setRecordingActive(true);
    tabs.setCurrentIndex(TabbedContentWidget::TabUploadFile);
    // Still recording: not a start.
    window->setRecordingActive(true);
    CHECK(tabs.currentIndex() == TabbedContentWidget::TabUploadFile);
    window->setRecordingActive(false);
    CHECK(tabs.currentIndex() == TabbedContentWidget::TabUploadFile);
    window->setRecordingActive(true);
    CHECK(tabs.currentIndex() == TabbedContentWidget::TabQuickRecord);
}

TEST_CASE("a walkthrough in the full window keeps its tab when its recording starts", "[widget]") {
    ShellFixture shell;
    ScreenRef walkthroughTab =
        addScreen(shell, fv::screen_key::walkthrough, fv::order::tab::walkthrough);
    auto window = shell.window();
    TabbedContentWidget& tabs = *window->tabbedContent();

    window->setWalkthroughActive(true);
    CHECK(tabs.currentIndex() == TabbedContentWidget::TabWalkthrough);
    window->setRecordingActive(true);
    CHECK(tabs.currentIndex() == TabbedContentWidget::TabWalkthrough);
    REQUIRE_FALSE(walkthroughTab->isNull());
    CHECK((*walkthroughTab)->calls.last() == QStringLiteral("setRecordingState:1,1"));
    CHECK(window->isWalkthroughActive());
}

TEST_CASE("in compact mode a walkthrough's recording stays on quick record", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    TabbedContentWidget& tabs = *window->tabbedContent();
    window->setCompactMode(true);
    window->setWalkthroughActive(true);
    CHECK(tabs.currentIndex() == TabbedContentWidget::TabQuickRecord);
    window->setRecordingActive(true);
    CHECK(window->isCompactMode());
    CHECK(tabs.currentIndex() == TabbedContentWidget::TabQuickRecord);
}

TEST_CASE("the recording state reaches quick record and the compact controller", "[widget]") {
    ShellFixture shell;
    auto quick = addQuickRecord(shell);
    auto window = shell.window();
    auto* controller = new fvtest::StandInController;
    window->setCompactController(controller);
    REQUIRE_FALSE(quick->isNull());

    window->setRecordingActive(true);
    CHECK((*quick)->calls.last() == QStringLiteral("setRecordingActive:1"));
    CHECK(controller->calls == QStringList{QStringLiteral("setRecordingState:1"),
                                           QStringLiteral("setStatus:Recording in progress...")});

    window->setRecordingActive(false);
    CHECK((*quick)->calls.last() == QStringLiteral("setRecordingActive:0"));
    CHECK(controller->calls.mid(2) == QStringList{QStringLiteral("setRecordingState:0"),
                                                  QStringLiteral("setStatus:Ready to record")});
}

TEST_CASE("a transient status reverts to ready after two seconds", "[widget]") {
    ShellFixture shell;
    auto quick = addQuickRecord(shell);
    auto window = shell.window();
    auto* controller = new fvtest::StandInController;
    window->setCompactController(controller);

    QElapsedTimer timer;
    timer.start();
    window->showTransientStatus(QStringLiteral("Copied to clipboard"));
    CHECK(window->status() == QStringLiteral("Copied to clipboard"));
    CHECK((*quick)->calls.last() == QStringLiteral("setStatus:Copied to clipboard"));
    CHECK(controller->calls.last() == QStringLiteral("setStatus:Copied to clipboard"));

    QTest::qWait(1000);
    CHECK(window->status() == QStringLiteral("Copied to clipboard"));
    REQUIRE(
        fvtest::waitUntil([&] { return window->status() == QStringLiteral("Ready to record"); }));
    // A two-second single shot is a coarse timer (as main_window.py's QTimer.singleShot(2000)),
    // which may fire up to 5 % early; Qt reckons its deadline on a whole-millisecond clock, so
    // the earliest firing reads 1899 ms on this finer one (seen under load).
    CHECK(timer.elapsed() >= 1900 - 1);
    CHECK((*quick)->calls.last() == QStringLiteral("setStatus:Ready to record"));
    CHECK(controller->calls.last() == QStringLiteral("setStatus:Ready to record"));

    window->showTransientStatus(QStringLiteral("Entry deleted"), std::chrono::milliseconds(50));
    CHECK(window->status() == QStringLiteral("Entry deleted"));
    CHECK(fvtest::waitUntil([&] { return window->status() == QStringLiteral("Ready to record"); },
                            std::chrono::milliseconds(1000)));
}

TEST_CASE("a status set before a keyed quick record tab exists reaches it as it opens",
          "[widget]") {
    ShellFixture shell;
    auto made = std::make_shared<QPointer<fvtest::StandInPage>>();
    shell.context().screens().define("quick", fv::order::tab::quick_record,
                                     QStringLiteral("Quick Record"),
                                     [made](QWidget* parent) -> QWidget* {
                                         *made = new fvtest::StandInPage({120}, parent);
                                         return made->data();
                                     });
    // Upload File was the last tab, so Quick Record is not built with the window.
    shell.settings().save_setting(
        "last_tab_index", TabbedContentWidget::savedTabId(TabbedContentWidget::TabUploadFile));
    auto window = shell.window();
    REQUIRE(made->isNull());
    window->setStatus(QStringLiteral("Loading speech engine..."));
    window->tabbedContent()->setCurrentIndex(TabbedContentWidget::TabQuickRecord);
    REQUIRE_FALSE(made->isNull());
    CHECK((*made)->calls.contains(QStringLiteral("setStatus:Loading speech engine...")));
}

TEST_CASE("the shell feature provides the main window, completed once features install",
          "[widget]") {
    ShellFixture shell;
    fv::ShellFeature feature;
    feature.provide(shell.context());
    MainWindow& window = shell.context().service<MainWindow>();
    CHECK_FALSE(window.setupComplete());
    CHECK(window.titleBar()->menuBar()->actions().isEmpty());
    CHECK(window.tabbedContent()->stack()->count() == 4);

    window.completeSetup();
    CHECK(window.setupComplete());
    CHECK(window.titleBar()->menuBar()->actions().size() == 3);
    window.tabbedContent()->setCurrentIndex(TabbedContentWidget::TabUploadFile);
    CHECK(shell.context().screens().existing(fv::ShellFeature::kUploadScreen) != nullptr);

    // A second completion does not add the menus again.
    window.completeSetup();
    CHECK(window.titleBar()->menuBar()->actions().size() == 3);
}
