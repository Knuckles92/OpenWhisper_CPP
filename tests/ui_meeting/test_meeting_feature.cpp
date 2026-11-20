// The Meeting Mode tab registered in the main window by the meeting feature's presenter
// (test_meeting_mode_tab.py's registration and window-height cases, test_meeting_intro_dialog.py's
// first visit, test_meeting_unsupported_platform.py's Windows tab, test_startup_visibility.py).

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QAbstractAnimation>
#include <QAction>
#include <QApplication>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabBar>
#include <QTest>

#include "presenter_support.h"
#include "settings/keys.h"
#include "ui_history/calendar_glyph_button.h"
#include "ui_history/history_calendar_dialog.h"
#include "ui_history/history_edge_tab.h"
#include "ui_history/history_sidebar.h"
#include "ui_meeting/meeting_intro_dialog.h"
#include "ui_meeting/meeting_mode_tab.h"
#include "ui_meeting/past_meetings_panel.h"
#include "ui_shell/page_fit.h"
#include "ui_shell/side_panel_host.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/tabbed_content.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::Json;
using fv::TabbedContentWidget;
using fvtest::PresenterRig;

// Until the window has no fit pending, no resize running and no panel moving.
void settle(fv::MainWindow& window) {
    QTest::qWait(150);
    (void)QTest::qWaitFor(
        [&window] {
            const auto* panel = dynamic_cast<const fv::SidePanel*>(window.sidePanel());
            const QAbstractAnimation* moving = panel != nullptr ? panel->widthAnimation() : nullptr;
            return !window.pageFit().scheduled() && !window.pageFit().animationRunning() &&
                   (moving == nullptr || moving->state() != QAbstractAnimation::Running);
        },
        5000);
    QApplication::processEvents();
}

fv::HistoryPanel& history(PresenterRig& rig) {
    return rig.context().service<fv::HistoryPanel>();
}

// The intro's runs, in place of its modal exec().
struct IntroRig {
    explicit IntroRig(Json saved, fv::meeting::PlatformInfo platform = fvtest::windowsPlatform())
        : rig(std::move(saved), hooks(std::move(platform))) {}

    fv::MeetingPresenter::Hooks hooks(fv::meeting::PlatformInfo platform) {
        fv::MeetingPresenter::Hooks made;
        made.runIntro = [this](fv::MeetingModeIntroDialog& dialog) {
            parents.push_back(dialog.parentWidget());
        };
        made.platform = std::move(platform);
        return made;
    }

    std::vector<QWidget*> parents;
    PresenterRig rig;
};

} // namespace

TEST_CASE("test_meeting_mode_is_the_last_tab (registered by the meeting feature)", "[widget]") {
    PresenterRig rig;
    auto* tabs = rig.window().tabbedContent();
    CHECK(TabbedContentWidget::TabMeetingMode == 3);
    CHECK(tabs->tabBar()->count() == 4);
    CHECK(tabs->tabBar()->tabText(TabbedContentWidget::TabMeetingMode) ==
          QStringLiteral("Meeting Mode"));
    auto* tab = rig.openMeetingTab();
    REQUIRE(tab != nullptr);
    CHECK(tabs->stack()->widget(TabbedContentWidget::TabMeetingMode) == tab);
}

TEST_CASE("test_sidebar_switches_to_past_meetings_for_meeting_mode", "[widget]") {
    PresenterRig rig;
    auto* sidebar = history(rig).sidebar();
    REQUIRE(sidebar != nullptr);
    auto* panel = rig.window().findChild<fv::PastMeetingsPanel*>();
    REQUIRE(panel != nullptr);
    CHECK(sidebar->page() == fv::HistorySidebar::Page::History);
    CHECK_FALSE(sidebar->contentWidget()->isHidden());
    CHECK(panel->isHidden());

    rig.window().tabbedContent()->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
    QApplication::processEvents();

    CHECK(sidebar->page() == fv::HistorySidebar::Page::Meetings);
    CHECK(sidebar->contentWidget()->isHidden());
    CHECK_FALSE(panel->isHidden());
    auto* action = rig.context().menus().action(fv::menu_id::sidePanel);
    REQUIRE(action != nullptr);
    CHECK(action->text() == QStringLiteral("Past Meetings"));
    auto* presenter = rig.window().findChild<fv::HistoryPresenter*>();
    REQUIRE(presenter != nullptr);
    CHECK(presenter->edgeTab()->toolTip().contains(QStringLiteral("Past Meetings")));
}

TEST_CASE("test_compact_mode_does_not_require_footer_meeting_strip", "[widget]") {
    PresenterRig rig;
    auto& window = rig.window();
    window.show();
    window.setCompactMode(true, false);
    CHECK(window.isCompactMode());
    CHECK_FALSE(window.tabbedContent()->isVisibleTo(&window));
    window.setCompactMode(false, false);
    CHECK(window.tabbedContent()->isVisibleTo(&window));
    window.tabbedContent()->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
    CHECK(window.tabbedContent()->currentIndex() == TabbedContentWidget::TabMeetingMode);
}

TEST_CASE("test_expanded_past_meetings_keeps_primary_controls_visible", "[widget]") {
    PresenterRig rig;
    auto& window = rig.window();
    window.maxUsableHeight = [] {
        return 1080;
    };
    window.show();
    window.resize(985, 800);
    window.tabbedContent()->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
    history(rig).sidebar()->setSidebarWidth(fv::HistorySidebar::kExpandedWidth);
    for (int i = 0; i < 5; ++i) {
        QApplication::processEvents();
    }
    settle(window);
    auto* tab = qobject_cast<fv::MeetingModeTab*>(window.page(TabbedContentWidget::TabMeetingMode));
    REQUIRE(tab != nullptr);
    QWidget* viewport = tab->scrollArea()->viewport();
    for (QWidget* control :
         {static_cast<QWidget*>(tab->cloudCheckbox()), static_cast<QWidget*>(tab->startButton())}) {
        INFO(control->objectName().toStdString());
        const QPoint topLeft = control->mapTo(viewport, QPoint(0, 0));
        CHECK(topLeft.x() >= 0);
        CHECK(topLeft.y() >= 0);
        CHECK(topLeft.x() + control->width() <= viewport->width());
        CHECK(topLeft.y() + control->height() <= viewport->height());
    }
    QTabBar* bar = window.tabbedContent()->tabBar();
    const QRect meeting = bar->tabRect(TabbedContentWidget::TabMeetingMode);
    CHECK(meeting.width() >= 120);
    CHECK(meeting.width() >= bar->tabRect(TabbedContentWidget::TabQuickRecord).width() - 2);
}

TEST_CASE("test_switch_to_and_from_meeting_mode_adjusts_height", "[widget]") {
    PresenterRig rig;
    auto& window = rig.window();
    window.maxUsableHeight = [] {
        return 1080;
    };
    window.show();
    window.tabbedContent()->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
    settle(window);
    window.tabbedContent()->setCurrentIndex(TabbedContentWidget::TabQuickRecord);
    settle(window);
    const int resting = window.height();

    window.tabbedContent()->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
    settle(window);
    CHECK(window.height() != resting);

    window.tabbedContent()->setCurrentIndex(TabbedContentWidget::TabQuickRecord);
    settle(window);
    CHECK(window.height() == resting);
}

TEST_CASE("test_first_visit_shows_intro_after_tab_opens", "[widget]") {
    IntroRig intro(Json{{"meeting_mode_intro_seen", false}});
    intro.rig.window().show();
    QApplication::processEvents();
    CHECK(intro.parents.empty());
    intro.rig.window().tabbedContent()->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
    REQUIRE(QTest::qWaitFor([&] { return !intro.parents.empty(); }, 2000));
    QApplication::processEvents();
    CHECK(intro.parents == std::vector<QWidget*>{&intro.rig.window()});
    CHECK(intro.rig.settings().get(fv::keys::meeting_mode_intro_seen));
}

TEST_CASE("test_seen_intro_does_not_show_again", "[widget]") {
    IntroRig intro(Json{{"meeting_mode_intro_seen", true}});
    intro.rig.window().show();
    intro.rig.window().tabbedContent()->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
    QTest::qWait(50);
    CHECK(intro.parents.empty());
}

TEST_CASE("test_active_meeting_does_not_block_on_intro", "[widget]") {
    IntroRig intro(Json{{"meeting_mode_intro_seen", false}});
    intro.rig.window().show();
    // Tray / hotkey start switches to this tab after capture is live.
    intro.rig.controls().active = true;
    intro.rig.window().tabbedContent()->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
    QTest::qWait(50);
    CHECK(intro.parents.empty());
    CHECK_FALSE(intro.rig.settings().get(fv::keys::meeting_mode_intro_seen));
}

TEST_CASE("test_restored_meeting_tab_shows_intro_on_first_show", "[widget]") {
    IntroRig intro(Json{
        {"meeting_mode_intro_seen", false},
        {"last_tab_index", TabbedContentWidget::savedTabId(TabbedContentWidget::TabMeetingMode)}});
    REQUIRE(intro.rig.window().tabbedContent()->currentIndex() ==
            TabbedContentWidget::TabMeetingMode);
    QApplication::processEvents();
    CHECK(intro.parents.empty());
    intro.rig.window().show();
    REQUIRE(QTest::qWaitFor([&] { return !intro.parents.empty(); }, 2000));
    QApplication::processEvents();
    CHECK(intro.parents.size() == 1);
}

TEST_CASE("test_locked_tab_waits_for_platform_ack", "[widget]") {
    IntroRig intro(
        Json{{"meeting_mode_intro_seen", false}, {"meeting_unsupported_platform_ack", false}},
        {"linux", "riscv64", ""});
    intro.rig.window().show();
    auto* tabs = intro.rig.window().tabbedContent();
    CHECK(tabs->meetingTabIsLocked());
    // Clicking the locked tab asks first; "No" keeps it locked and shows nothing.
    fvtest::answerNextBox(&intro.rig.window(), QMessageBox::No);
    tabs->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
    QTest::qWait(50);
    CHECK(intro.parents.empty());
    CHECK(tabs->meetingTabIsLocked());
}

TEST_CASE("test_windows_tab_is_not_muted_or_locked", "[widget]") {
    PresenterRig rig(Json{{"meeting_unsupported_platform_ack", false}});
    auto* tabs = rig.window().tabbedContent();
    CHECK_FALSE(tabs->meetingTabIsLocked());
    CHECK(tabs->tabBar()->property("unsupportedMeeting") != QVariant(true));
    CHECK(tabs->tabBar()->tabToolTip(TabbedContentWidget::TabMeetingMode) ==
          QStringLiteral("Meeting Mode is in beta. Transcripts and insights may be inaccurate."));
}

TEST_CASE("test_startup_only_shows_the_requested_main_window", "[widget]") {
    struct ShowObserver : QObject {
        std::vector<QWidget*> shown;
        bool eventFilter(QObject* object, QEvent* event) override {
            if (event->type() == QEvent::Show) {
                if (auto* widget = qobject_cast<QWidget*>(object); widget && widget->isWindow()) {
                    shown.push_back(widget);
                }
            }
            return false;
        }
    };
    for (const int lastTab : {0, 1, 2, 3}) {
        for (const bool developer : {false, true}) {
            DYNAMIC_SECTION("tab " << lastTab << " developer " << developer) {
                ShowObserver observer;
                qApp->installEventFilter(&observer);
                {
                    PresenterRig rig(
                        Json{{"last_tab_index", TabbedContentWidget::savedTabId(lastTab)},
                             {"developer_mode", developer}});
                    QApplication::processEvents();
                    CHECK(observer.shown.empty());
                    CHECK_FALSE(rig.window().isVisible());
                    rig.window().show();
                    QApplication::processEvents();
                    CHECK(observer.shown == std::vector<QWidget*>{&rig.window()});
                    CHECK(rig.window().tabbedContent()->currentIndex() == lastTab);
                    auto* tab = rig.openMeetingTab();
                    REQUIRE(tab != nullptr);
                    // AI insights start off for every meeting, so the brief starts hidden.
                    CHECK_FALSE(tab->briefPanel()->isVisibleTo(tab));
                    CHECK_FALSE(tab->platformHint()->isVisibleTo(tab));
                    tab->cloudCheckbox()->setChecked(true);
                    QApplication::processEvents();
                    CHECK(tab->briefPanel()->isVisibleTo(tab));
                    CHECK(observer.shown == std::vector<QWidget*>{&rig.window()});
                }
                qApp->removeEventFilter(&observer);
            }
        }
    }
}

TEST_CASE("test_past_meetings_header_opens_the_calendar_too", "[widget]") {
    PresenterRig rig;
    auto* panel = rig.window().findChild<fv::PastMeetingsPanel*>();
    REQUIRE(panel != nullptr);
    REQUIRE(panel->calendarButton() != nullptr);
    CHECK(rig.window().findChild<fv::HistoryCalendarDialog*>() == nullptr);
    panel->calendarButton()->click();
    REQUIRE(QTest::qWaitFor(
        [&] { return rig.window().findChild<fv::HistoryCalendarDialog*>() != nullptr; }, 2000));
    rig.window().findChild<fv::HistoryCalendarDialog*>()->close();
}
