// The History calendar from the main window: opening it, keeping it current, opening records
// (OpenWhisper's tests/test_history_calendar_wiring.py). The calendar reads a fixed September
// through the context's CalendarReads, as Python's test patches CalendarSource with FakeSource.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QAction>
#include <QKeySequence>
#include <QPushButton>
#include <QTimer>

#include "appkit/app_context.h"
#include "appkit/registries.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/fake_clock.h"
#include "calendar_test_support.h"
#include "history_ui_support.h"
#include "settings/testing/memory_store.h"
#include "ui_history/calendar_glyph_button.h"
#include "ui_history/history_calendar_dialog.h"
#include "ui_history/history_entry_dialog.h"
#include "ui_history/history_feature.h"
#include "ui_history/history_sidebar.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_widgets/buttons.h"

using fv::HistoryCalendarDialog;
using fv::HistoryFeature;
using fv::HistoryPanel;
using fv::MainWindow;

namespace {

// The shell and History as Bootstrap builds them, with the calendar's reads in the context.
class WiringFixture {
public:
    explicit WiringFixture(
        std::shared_ptr<fv::HistoryStore> store = std::make_shared<fvtest::FakeStore>())
        : paths_(fv::Paths::resolve(fv::identity(), fv::system_env())), ui_(qApp),
          clock_(fvtest::todayNoon()), context_(services()) {
        shell_.provide(context_);
        history_.provide(context_);
        context_.provide<fv::HistoryStore>(std::move(store));
        context_.provide<fv::CalendarReads>(reads_);
        shell_.install(context_);
        history_.install(context_);
        window().availableScreenRect = [] {
            return QRect(0, 0, 1920, 1080);
        };
    }

    ~WiringFixture() {
        if (HistoryCalendarDialog* calendar = presenter()->calendar()) {
            calendar->hide();
        }
    }

    MainWindow& window() { return context_.service<MainWindow>(); }
    HistoryPanel& panel() { return context_.service<HistoryPanel>(); }
    fv::HistorySidebar* sidebar() { return panel().sidebar(); }
    fv::HistoryPresenter* presenter() { return window().findChild<fv::HistoryPresenter*>(); }
    HistoryCalendarDialog* calendar() { return presenter()->calendar(); }
    fv::MenuRegistry& menus() { return context_.menus(); }
    fvtest::Io& io() { return io_; }

    // Opens the calendar and lets its reads land.
    HistoryCalendarDialog* openCalendar() {
        panel().openCalendar();
        io_.drain();
        return calendar();
    }

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

    std::shared_ptr<fvtest::FakeReads> reads_ =
        std::make_shared<fvtest::FakeReads>(fvtest::september());
    fv::testkit::TempDataRoot root_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::FakeClock clock_;
    fvtest::Io io_;
    fv::AppContext context_;
    fv::ShellFeature shell_;
    HistoryFeature history_;
};

} // namespace

TEST_CASE("the sidebar and menu open one calendar", "[test_history_calendar_wiring][widget]") {
    // from tests/test_history_calendar_wiring.py::test_the_sidebar_and_menu_open_one_calendar
    WiringFixture fixture;
    fixture.sidebar()->calendarButton()->click();
    fixture.io().drain();
    HistoryCalendarDialog* calendar = fixture.calendar();
    REQUIRE(calendar != nullptr);
    CHECK(calendar->isVisible());
    CHECK(calendar->cards().size() == 3);
    fixture.panel().openCalendar();
    fixture.io().drain();
    CHECK(fixture.calendar() == calendar);

    QAction* action = fixture.menus().action(fv::menu_id::historyCalendar);
    REQUIRE(action != nullptr);
    CHECK(action->text() == QStringLiteral("History Calendar"));
    CHECK(action->shortcut() == QKeySequence(QStringLiteral("Ctrl+Shift+D")));
    action->trigger();
    fixture.io().drain();
    CHECK(fixture.calendar() == calendar);
}

TEST_CASE("history changes reach an open calendar", "[test_history_calendar_wiring][widget]") {
    // from tests/test_history_calendar_wiring.py::test_history_changes_reach_an_open_calendar
    WiringFixture fixture;
    HistoryCalendarDialog* calendar = fixture.openCalendar();
    REQUIRE(calendar != nullptr);
    QTimer* refresh = calendar->refreshTimer();
    REQUIRE_FALSE(refresh->isActive());

    // refresh_history, refresh_past_meetings and _on_history_entry_deleted each ask it to
    // refresh soon.
    fixture.panel().refresh();
    CHECK(refresh->isActive());
    refresh->stop();
    fixture.panel().refreshPastMeetings();
    CHECK(refresh->isActive());
    refresh->stop();
    Q_EMIT fixture.sidebar()->entryDeleted(QStringLiteral("x"));
    CHECK(refresh->isActive());
    refresh->stop();
}

TEST_CASE("records open above the calendar", "[test_history_calendar_wiring][widget]") {
    // from tests/test_history_calendar_wiring.py::test_records_open_above_the_calendar
    WiringFixture fixture;
    HistoryCalendarDialog* calendar = fixture.openCalendar();
    REQUIRE(calendar != nullptr);
    QWidget* parent = nullptr;
    std::string shown;
    fixture.presenter()->runDialog = [&](fv::HistoryEntryDialog& dialog) {
        parent = dialog.parentWidget();
        shown = dialog.entry().id;
    };
    fv::HistoryEntry entry = fvtest::makeEntry("From the calendar");
    entry.id = "entry-1234";
    Q_EMIT calendar->entryRequested(entry);
    CHECK(parent == calendar);
    CHECK(shown == "entry-1234");

    QStringList meetings;
    QObject::connect(&fixture.window(), &MainWindow::pastMeetingRequested,
                     [&meetings](const QString& id) { meetings.append(id); });
    Q_EMIT calendar->meetingRequested(QStringLiteral("m1"));
    CHECK(meetings == QStringList{QStringLiteral("m1")});
}

TEST_CASE("an edit in the entry dialog refreshes both lists",
          "[test_history_calendar_wiring][widget]") {
    // from
    // tests/test_history_calendar_wiring.py::test_an_edit_in_the_entry_dialog_refreshes_both_lists
    fvtest::LocalHistory history;
    auto entry = history.addCleaned();
    const bool closeEarly = GENERATE(false, true);
    WiringFixture fixture(history.store());
    fixture.panel().toggle();
    fixture.io().drain();
    HistoryCalendarDialog* calendar = fixture.openCalendar();
    REQUIRE(calendar != nullptr);
    const std::uint64_t loads = fixture.sidebar()->loadGeneration();
    fixture.presenter()->runDialog = [&](fv::HistoryEntryDialog& dialog) {
        dialog.rawButton()->click();
        dialog.useVersionButton()->click();
        if (!closeEarly) {
            fixture.io().drain();
        }
    };
    Q_EMIT calendar->entryRequested(entry);
    if (closeEarly) {
        CHECK(fixture.sidebar()->loadGeneration() == loads);
        fixture.io().drain();
    }
    REQUIRE(history.get(entry.id));
    CHECK(history.get(entry.id)->text == "um hello world");
    CHECK(fixture.sidebar()->loadGeneration() > loads);
    CHECK(calendar->refreshTimer()->isActive());
    calendar->refreshTimer()->stop();
}
