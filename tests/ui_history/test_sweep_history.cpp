// Sweep H2: open items of the History panel and the entry dialog.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>

#include <QApplication>
#include <QRect>
#include <QWidget>

#include "appkit/app_context.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/fake_clock.h"
#include "history_ui_support.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_history/history_feature.h"
#include "ui_history/history_sidebar.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_widgets/tabbed_content.h"

namespace {

// A Past Meetings or Walkthroughs panel: counts the sidebar's refresh() calls.
class CountingPage : public QWidget {
    Q_OBJECT
public:
    int refreshes = 0;
    Q_INVOKABLE void refresh() { ++refreshes; }
};

// The shell and History as Bootstrap builds them.
class ShellRig {
public:
    ShellRig()
        : paths_(fv::Paths::resolve(fv::identity(), fv::system_env())), ui_(qApp),
          context_(services()) {
        shell_.provide(context_);
        history_.provide(context_);
        context_.provide<fv::HistoryStore>(std::make_shared<fvtest::FakeStore>());
        shell_.install(context_);
        history_.install(context_);
        window().availableScreenRect = [] {
            return QRect(0, 0, 1920, 1080);
        };
        // Bootstrap's step after every feature installed: the tabs take their screens and the
        // tab changes are connected.
        window().completeSetup();
    }

    fv::MainWindow& window() { return context_.service<fv::MainWindow>(); }
    fv::HistoryPanel& panel() { return context_.service<fv::HistoryPanel>(); }
    fv::HistorySidebar* sidebar() { return panel().sidebar(); }

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.clock = &clock_;
        made.ui = &ui_;
        made.io = &io_.executor();
        return made;
    }

    fv::testkit::TempDataRoot root_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::FakeClock clock_;
    fvtest::Io io_;
    fv::AppContext context_;
    fv::ShellFeature shell_;
    fv::HistoryFeature history_;
};

} // namespace

// open item: "HistoryPanel::refreshPastMeetings refreshes the sidebar when its page is Meetings,
// where Python checks that the main window's tab is Meeting Mode". They agree: the sidebar's page
// is set only from the main window's tab (MainWindow::sidePanelPageRequested on every tab change,
// and the current tab when the presenter is built), as _on_tab_changed's set_page is.
TEST_CASE("past meetings refresh the sidebar exactly while Meeting Mode is the tab",
          "[sweep_history][widget]") {
    ShellRig rig;
    fv::HistorySidebar* sidebar = rig.sidebar();
    REQUIRE(sidebar != nullptr);
    auto* meetings = new CountingPage;
    auto* walkthroughs = new CountingPage;
    sidebar->setPageWidget(fv::HistorySidebar::Page::Meetings, meetings);
    sidebar->setPageWidget(fv::HistorySidebar::Page::Walkthroughs, walkthroughs);
    sidebar->expand();
    QApplication::processEvents();
    REQUIRE(sidebar->isExpanded());

    fv::TabbedContentWidget* tabs = rig.window().tabbedContent();
    const int order[] = {
        fv::TabbedContentWidget::TabMeetingMode, fv::TabbedContentWidget::TabQuickRecord,
        fv::TabbedContentWidget::TabWalkthrough, fv::TabbedContentWidget::TabMeetingMode,
        fv::TabbedContentWidget::TabUploadFile};
    for (const int tab : order) {
        tabs->setCurrentIndex(tab);
        QApplication::processEvents();
        REQUIRE(tabs->currentIndex() == tab);
        const bool meetingTab = tab == fv::TabbedContentWidget::TabMeetingMode;
        CHECK((sidebar->page() == fv::HistorySidebar::Page::Meetings) == meetingTab);

        const int before = meetings->refreshes;
        const int walkthroughsBefore = walkthroughs->refreshes;
        rig.panel().refreshPastMeetings();
        CHECK(meetings->refreshes - before == (meetingTab ? 1 : 0));
        CHECK(walkthroughs->refreshes == walkthroughsBefore);
    }
}

#include "test_sweep_history.moc"
