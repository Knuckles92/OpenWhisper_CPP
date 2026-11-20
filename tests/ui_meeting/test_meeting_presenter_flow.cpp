// The desktop presenter's meeting flow outside the Meeting Mode tab (ui_controller.py's
// on_meeting_state_changed, show_meeting_recovery_scan_error, _on_meeting_cloud_toggled;
// main_window.py's _schedule_meeting_mode_intro): a start from the tray or the hotkey brings the
// tab forward, the first-visit intro shows once and never over a meeting or meeting setup, a
// finished meeting refreshes Past Meetings, a failed recovery scan offers its retry, and the AI
// insights switch reaches the runtime only with consent.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QMessageBox>
#include <QTabBar>
#include <QTest>
#include <QTimer>

#include "meeting_agent/agent_settings.h"
#include "presenter_support.h"
#include "settings/keys.h"
#include "ui_history/history_sidebar.h"
#include "ui_meeting/meeting_intro_dialog.h"
#include "ui_meeting/meeting_mode_tab.h"
#include "ui_meeting/past_meetings_panel.h"
#include "ui_widgets/tabbed_content.h"

namespace {

using fv::Json;
using fv::TabbedContentWidget;
using fvtest::PresenterRig;

// A presenter whose intro and meeting setup are recorded instead of shown.
struct FlowRig {
    explicit FlowRig(Json saved) : rig(std::move(saved), hooks()) {}

    fv::MeetingPresenter::Hooks hooks() {
        fv::MeetingPresenter::Hooks made;
        made.runIntro = [this](fv::MeetingModeIntroDialog& dialog) {
            intros.push_back(dialog.parentWidget());
        };
        made.openSetup = [this](bool starting) -> std::optional<std::string> {
            if (duringSetup) {
                duringSetup();
            }
            setups.push_back(starting);
            return std::nullopt;
        };
        return made;
    }

    std::vector<QWidget*> intros;
    std::vector<bool> setups;
    std::function<void()> duringSetup;
    PresenterRig rig;
};

fv::SystemTray& trayOf(PresenterRig& rig, std::unique_ptr<fv::SystemTray>& holder) {
    fv::TrayEnvironment environment;
    environment.available = [] {
        return false;
    };
    environment.showIcon = [](QSystemTrayIcon&) {
    };
    holder = std::make_unique<fv::SystemTray>(rig.window(), environment);
    rig.presenter().attachTray(*holder);
    return *holder;
}

fv::MeetingModeTab* meetingTab(PresenterRig& rig) {
    return rig.window().findChild<fv::MeetingModeTab*>();
}

// Accepts every modal dialog that comes up until `until` is destroyed.
void acceptDialogs(QObject* until, bool accept) {
    auto* timer = new QTimer(until);
    QObject::connect(timer, &QTimer::timeout, [accept] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            dialog->done(accept ? QDialog::Accepted : QDialog::Rejected);
        }
    });
    timer->start(10);
}

} // namespace

TEST_CASE("a meeting started from the tray or the hotkey brings Meeting Mode forward",
          "[MW-02][widget]") {
    for (const std::string seam : {"tray", "hotkey"}) {
        DYNAMIC_SECTION(seam) {
            FlowRig flow(Json{{"meeting_mode_intro_seen", false}});
            auto& rig = flow.rig;
            rig.window().show();
            auto* tabs = rig.window().tabbedContent();
            tabs->setCurrentIndex(TabbedContentWidget::TabQuickRecord);
            QApplication::processEvents();
            // Meeting Mode was never visited: its page is not built yet.
            CHECK(meetingTab(rig) == nullptr);

            std::unique_ptr<fv::SystemTray> tray;
            if (seam == "tray") {
                Q_EMIT trayOf(rig, tray).meetingToggleRequested();
            } else {
                // MeetingRuntime::toggle_from_hotkey asks for the readiness gates this way.
                rig.controls().meeting_platform_ack_requested.emit();
            }
            REQUIRE(QTest::qWaitFor([&] { return !rig.controls().named("start").empty(); }));
            CHECK(rig.controls().named("start") ==
                  std::vector<std::string>{"start_meeting false|auto|"});
            // Asking for a start moves nothing; the runtime's state does.
            CHECK(tabs->currentIndex() == TabbedContentWidget::TabQuickRecord);

            rig.controls().active = true;
            rig.controls().meeting_state_changed.emit(
                Json{{"active", true}, {"status", "recording"}, {"meeting_id", "m-1"}});
            QApplication::processEvents();
            CHECK(tabs->currentIndex() == TabbedContentWidget::TabMeetingMode);
            auto* tab = meetingTab(rig);
            REQUIRE(tab != nullptr);
            CHECK(tab->isMeetingActive());
            CHECK_FALSE(tabs->tabBar()->isTabEnabled(TabbedContentWidget::TabQuickRecord));
            // Tray / hotkey start switches to this tab after capture is live: no intro over it.
            QTest::qWait(50);
            CHECK(flow.intros.empty());
            CHECK_FALSE(rig.settings().get(fv::keys::meeting_mode_intro_seen));
        }
    }
}

TEST_CASE("the first visit to the unlocked Meeting tab shows the intro once, never over meeting "
          "setup",
          "[A01-59][widget]") {
    FlowRig flow(Json{{"meeting_mode_intro_seen", false}, {"meeting_setup_complete", false}});
    auto& rig = flow.rig;
    auto* tabs = rig.window().tabbedContent();
    tabs->setCurrentIndex(TabbedContentWidget::TabQuickRecord);
    rig.window().show();
    QApplication::processEvents();

    // Meeting setup is open (the tray's first start runs it) when the tab comes up.
    flow.duringSetup = [&] {
        tabs->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
        QTest::qWait(50);
    };
    std::unique_ptr<fv::SystemTray> tray;
    Q_EMIT trayOf(rig, tray).meetingToggleRequested();
    CHECK(flow.setups == std::vector<bool>{true});
    CHECK(flow.intros.empty());
    CHECK_FALSE(rig.settings().get(fv::keys::meeting_mode_intro_seen));

    // The next visit, setup closed and no meeting: the intro, once.
    tabs->setCurrentIndex(TabbedContentWidget::TabQuickRecord);
    QApplication::processEvents();
    tabs->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
    REQUIRE(QTest::qWaitFor([&] { return !flow.intros.empty(); }, 2000));
    CHECK(flow.intros == std::vector<QWidget*>{&rig.window()});
    CHECK(rig.settings().get(fv::keys::meeting_mode_intro_seen));

    tabs->setCurrentIndex(TabbedContentWidget::TabQuickRecord);
    QApplication::processEvents();
    tabs->setCurrentIndex(TabbedContentWidget::TabMeetingMode);
    rig.window().hide();
    rig.window().show();
    QTest::qWait(50);
    CHECK(flow.intros.size() == 1);
}

TEST_CASE("a finished meeting and the runtime's request refresh Past Meetings",
          "[A03-71][widget]") {
    PresenterRig rig;
    rig.window().show();
    REQUIRE(rig.openMeetingTab() != nullptr);
    auto* sidebar = rig.context().service<fv::HistoryPanel>().sidebar();
    REQUIRE(sidebar != nullptr);
    sidebar->expand();
    auto* panel = rig.window().findChild<fv::PastMeetingsPanel*>();
    REQUIRE(panel != nullptr);
    QApplication::processEvents();

    auto generation = panel->loadGeneration();
    const auto refreshed = [&] {
        QApplication::processEvents();
        const bool moved = panel->loadGeneration() != generation;
        generation = panel->loadGeneration();
        return moved;
    };
    const auto state = [&](Json patch) {
        rig.controls().meeting_state_changed.emit(std::move(patch));
    };
    state(Json{{"active", true}, {"status", "recording"}, {"meeting_id", "m-1"}});
    CHECK_FALSE(refreshed());
    // Capture stopped but the meeting is still finishing: nothing new to list yet.
    state(Json{{"active", false}, {"status", "finalizing"}, {"meeting_id", "m-1"}});
    CHECK_FALSE(refreshed());
    for (const char* status : {"ended", "failed", "needs_recovery", "canceled"}) {
        state(Json{{"active", false}, {"status", status}, {"meeting_id", "m-1"}});
        CHECK(refreshed());
    }
    // A patch without "active" is no end of capture.
    state(Json{{"status", "ended"}});
    CHECK_FALSE(refreshed());

    rig.controls().past_meetings_refresh_requested.emit();
    CHECK(refreshed());
}

TEST_CASE("test_recovery_scan_failure_routes_retry_to_runtime", "[widget]") {
    PresenterRig rig;
    rig.window().show();
    auto& calls = rig.controls().calls;
    const auto box = [&] {
        return rig.window().findChild<QMessageBox*>();
    };
    const auto button = [](QMessageBox* dialog, const QString& text) -> QAbstractButton* {
        for (auto* candidate : dialog->buttons()) {
            if (candidate->text() == text) {
                return candidate;
            }
        }
        return nullptr;
    };

    rig.controls().meeting_recovery_scan_failed.emit("database unavailable");
    REQUIRE(QTest::qWaitFor([&] { return box() != nullptr; }));
    auto* warning = box();
    CHECK(warning->windowTitle() == QStringLiteral("Meeting Recovery"));
    CHECK(warning->text().contains(QStringLiteral("database unavailable")));
    CHECK(warning->informativeText() ==
          QStringLiteral("Saved meeting audio remains on this computer. Retry the scan to find "
                         "interrupted meetings."));
    CHECK(warning->icon() == QMessageBox::Warning);
    CHECK_FALSE(warning->isModal());
    CHECK(rig.window().status() == QStringLiteral("Meeting recovery scan failed"));
    auto* retry = button(warning, QStringLiteral("Retry scan"));
    auto* later = button(warning, QStringLiteral("Later"));
    REQUIRE(retry != nullptr);
    REQUIRE(later != nullptr);
    CHECK(warning->buttonRole(retry) == QMessageBox::ActionRole);
    CHECK(warning->buttonRole(later) == QMessageBox::RejectRole);

    // A second failure while it is up updates the same warning.
    rig.controls().meeting_recovery_scan_failed.emit("database still unavailable");
    QApplication::processEvents();
    CHECK(rig.window().findChildren<QMessageBox*>().size() == 1);
    CHECK(warning->text().contains(QStringLiteral("still")));

    // Later retries nothing.
    later->click();
    REQUIRE(QTest::qWaitFor([&] { return box() == nullptr; }));
    CHECK(calls.empty());

    // Retry scan routes to the runtime.
    rig.controls().meeting_recovery_scan_failed.emit("database unavailable");
    REQUIRE(QTest::qWaitFor([&] { return box() != nullptr; }));
    button(box(), QStringLiteral("Retry scan"))->click();
    REQUIRE(QTest::qWaitFor([&] { return !calls.empty(); }));
    CHECK(calls == std::vector<std::string>{"retry_recovery_scan"});
}

TEST_CASE("the AI insights switch reaches the runtime only with consent", "[A03-97][widget]") {
    PresenterRig rig;
    auto* tab = rig.openMeetingTab();
    REQUIRE(tab != nullptr);
    const auto route = fv::meeting_agent::resolve_meeting_agent_route(*rig.settings().snapshot());
    auto& calls = rig.controls().calls;

    // No meeting: nothing to switch.
    Q_EMIT tab->cloudToggled(true);
    CHECK(calls.empty());

    rig.controls().active = true;
    rig.controls().destination = std::make_pair(std::string("m-1"), route);
    rig.controls().meeting_state_changed.emit(Json{{"active", true},
                                                   {"status", "recording"},
                                                   {"meeting_id", "m-1"},
                                                   {"cloud_enabled", false}});
    QApplication::processEvents();

    {
        QObject declining;
        acceptDialogs(&declining, false);
        Q_EMIT tab->cloudToggled(true);
    }
    CHECK(rig.controls().named("set_live_intelligence").empty());

    {
        QObject accepting;
        acceptDialogs(&accepting, true);
        Q_EMIT tab->cloudToggled(true);
    }
    CHECK(rig.controls().named("set_live_intelligence") ==
          std::vector<std::string>{"set_live_intelligence m-1|on"});
    CHECK(rig.settings().get(fv::keys::meeting_cloud_consent_given));

    calls.clear();
    rig.controls().meeting_state_changed.emit(Json{{"cloud_enabled", true}});
    QApplication::processEvents();
    // Turning it off asks nothing.
    Q_EMIT tab->cloudToggled(false);
    CHECK(calls == std::vector<std::string>{"set_live_intelligence m-1|off"});
}
