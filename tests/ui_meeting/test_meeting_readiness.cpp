// The controller side of Meeting Mode (ui_controller.py's meeting handlers): the start
// readiness seams shared by the tab, the tray and the hotkey
// (test_meeting_start_readiness_seams.py, test_meeting_setup.py), the End confirmation, the tab's
// one-line forwards, the state payload, the copy statuses and the meeting error's restart offer.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QMessageBox>
#include <QTabBar>
#include <QTest>

#include "appkit/order.h"
#include "meeting/setup_check.h"
#include "presenter_support.h"
#include "settings/keys.h"
#include "ui_history/history_sidebar.h"
#include "ui_meeting/meeting_mode_tab.h"
#include "ui_meeting/past_meetings_panel.h"
#include "ui_meeting/recent_meeting_row.h"
#include "ui_shell/bootstrap.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/tabbed_content.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::Json;
using fvtest::PresenterRig;

// A rig whose first start still needs setup; the setup dialog answers `policy`.
struct SetupRig {
    explicit SetupRig(std::optional<std::string> answer, bool complete = false,
                      fv::meeting::PlatformInfo platform = fvtest::windowsPlatform())
        : policy(std::move(answer)),
          rig(Json{{"meeting_setup_complete", complete}}, hooks(platform)) {}

    fv::MeetingPresenter::Hooks hooks(fv::meeting::PlatformInfo platform) {
        fv::MeetingPresenter::Hooks made;
        made.openSetup = [this](bool starting) {
            setups.push_back(starting);
            return policy;
        };
        made.platform = std::move(platform);
        return made;
    }

    std::optional<std::string> policy;
    std::vector<bool> setups;
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

void startFrom(SetupRig& setup, const std::string& seam) {
    if (seam == "tab") {
        auto* tab = setup.rig.openMeetingTab();
        REQUIRE(tab != nullptr);
        Q_EMIT tab->startRequested(true, QStringLiteral("Plan the launch"));
    } else {
        std::unique_ptr<fv::SystemTray> tray;
        Q_EMIT trayOf(setup.rig, tray).meetingToggleRequested();
    }
}

std::string started(const std::string& seam, const std::string& policy) {
    // The tab passes its switch and brief; the tray starts with AI insights off, since FioriVoce
    // asks for them per meeting and remembers no earlier choice.
    return seam == "tab" ? "start_meeting true|" + policy + "|Plan the launch"
                         : "start_meeting false|" + policy + "|";
}

} // namespace

TEST_CASE("test_start_seams_forward_ready_policy_once", "[widget]") {
    for (const std::string seam : {"tab", "tray"}) {
        DYNAMIC_SECTION(seam) {
            SetupRig setup("required");
            startFrom(setup, seam);
            CHECK(setup.setups == std::vector<bool>{true});
            CHECK(setup.rig.controls().named("start") ==
                  std::vector<std::string>{started(seam, "required")});
        }
    }
}

TEST_CASE("test_start_seams_cancel_starts_nothing", "[widget]") {
    for (const std::string seam : {"tab", "tray"}) {
        DYNAMIC_SECTION(seam) {
            SetupRig setup(std::nullopt);
            startFrom(setup, seam);
            CHECK(setup.setups.size() == 1);
            CHECK(setup.rig.controls().named("start").empty());
        }
    }
}

TEST_CASE("test_start_seams_forward_mic_only_disabled", "[widget]") {
    for (const std::string seam : {"tab", "tray"}) {
        DYNAMIC_SECTION(seam) {
            SetupRig setup("disabled");
            startFrom(setup, seam);
            CHECK(setup.rig.controls().named("start") ==
                  std::vector<std::string>{started(seam, "disabled")});
        }
    }
}

TEST_CASE("test_first_start_runs_shared_setup (Windows rows)", "[widget]") {
    struct Row {
        bool complete;
        std::optional<std::string> setupPolicy;
        std::optional<std::string> policy;
    };
    for (const Row& row : {Row{false, std::nullopt, std::nullopt}, Row{false, "auto", "auto"},
                           Row{true, std::nullopt, "auto"}}) {
        DYNAMIC_SECTION("complete " << row.complete << " setup "
                                    << row.setupPolicy.value_or("cancel")) {
            SetupRig setup(row.setupPolicy, row.complete);
            startFrom(setup, "tab");
            CHECK(setup.setups == (row.complete ? std::vector<bool>{} : std::vector<bool>{true}));
            CHECK(setup.rig.controls().named("start") ==
                  (row.policy ? std::vector<std::string>{started("tab", *row.policy)}
                              : std::vector<std::string>{}));
        }
    }
}

TEST_CASE("test_demo_uses_platform_ack_without_audio_readiness", "[widget]") {
    SECTION("Windows needs no acknowledgement") {
        SetupRig setup("required");
        auto* tab = setup.rig.openMeetingTab();
        Q_EMIT tab->demoRequested(false);
        CHECK(setup.setups.empty());
        CHECK(setup.rig.controls().named("start") ==
              std::vector<std::string>{"start_demo_meeting false|disabled"});
    }
    SECTION("an unsupported platform asks once, then the demo starts without setup") {
        SetupRig setup("required", false, {"linux", "riscv64", ""});
        auto* tab = setup.rig.openMeetingTab();
        fvtest::answerNextBox(&setup.rig.window(), QMessageBox::Yes);
        Q_EMIT tab->demoRequested(false);
        CHECK(setup.setups.empty());
        CHECK(setup.rig.controls().named("start") ==
              std::vector<std::string>{"start_demo_meeting false|disabled"});
        CHECK(setup.rig.settings().get(fv::keys::meeting_unsupported_platform_ack));
    }
}

TEST_CASE("test_demo_cancelled_by_platform_ack_starts_nothing", "[widget]") {
    SetupRig setup("required", false, {"linux", "riscv64", ""});
    setup.rig.settings().save_setting("meeting_unsupported_platform_ack", false);
    auto* tab = setup.rig.openMeetingTab();
    fvtest::answerNextBox(&setup.rig.window(), QMessageBox::No);
    Q_EMIT tab->demoRequested(false);
    CHECK(setup.setups.empty());
    CHECK(setup.rig.controls().calls.empty());
}

TEST_CASE("test_active_tray_end_requires_confirmation", "[widget]") {
    for (const auto answer : {QMessageBox::Yes, QMessageBox::No}) {
        DYNAMIC_SECTION((answer == QMessageBox::Yes ? "Yes" : "No")) {
            SetupRig setup("required");
            setup.rig.controls().active = true;
            std::unique_ptr<fv::SystemTray> tray;
            auto& icon = trayOf(setup.rig, tray);
            QString title;
            QString text;
            QTimer::singleShot(0, &setup.rig.window(), [&] {
                if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    title = box->windowTitle();
                    text = box->text();
                    CHECK(box->defaultButton() == box->button(QMessageBox::No));
                    box->button(answer)->click();
                }
            });
            Q_EMIT icon.meetingToggleRequested();
            CHECK(title == QStringLiteral("End meeting?"));
            CHECK(text == QStringLiteral("End this meeting now? Audio capture will stop "
                                         "immediately. FioriVoce will keep working in the "
                                         "background to finish the transcript and report."));
            CHECK(setup.rig.controls().named("end").size() == (answer == QMessageBox::Yes ? 1 : 0));
            CHECK(setup.setups.empty());
            CHECK(setup.rig.controls().named("start").empty());
        }
    }
}

TEST_CASE("the tab's End asks the same question and an idle End asks nothing", "[widget]") {
    PresenterRig rig;
    auto* tab = rig.openMeetingTab();
    Q_EMIT tab->endRequested();
    CHECK(QApplication::activeModalWidget() == nullptr);
    CHECK(rig.controls().named("end").empty());
    rig.controls().active = true;
    fvtest::answerNextBox(&rig.window(), QMessageBox::Yes);
    Q_EMIT tab->endRequested();
    CHECK(rig.controls().named("end").size() == 1);
}

TEST_CASE("test_hotkey_controller_uses_readiness_once", "[widget]") {
    SetupRig setup("auto");
    setup.rig.controls().meeting_platform_ack_requested.emit();
    REQUIRE(QTest::qWaitFor([&] { return !setup.rig.controls().calls.empty(); }));
    CHECK(setup.setups == std::vector<bool>{true});
    CHECK(setup.rig.controls().calls == std::vector<std::string>{"start_meeting false|auto|"});
}

TEST_CASE("test_inspect_remote_does_not_require_local_speech_models", "[unit]") {
    fv::meeting::SetupDeps deps;
    deps.settings = [] {
        return Json{{"meeting_speech_separate", true}, {"meeting_asr_source", "remote"}};
    };
    deps.speaker_model_path = [] {
        return std::optional<std::filesystem::path>("speaker.onnx");
    };
    int cached = 0;
    deps.is_model_cached = [&cached](std::string_view) {
        ++cached;
        return false;
    };
    deps.resolve_runtime = [](std::string_view, std::string_view) -> std::string {
        FAIL("a remote source resolves no local runtime");
        return {};
    };
    CHECK(fv::meeting::inspect_models(deps).missing().empty());
    CHECK(cached == 0);
}

TEST_CASE("Quick Record holds meeting setup with a status line", "[widget]") {
    SetupRig setup("auto");
    auto* tab = setup.rig.openMeetingTab();
    REQUIRE(tab != nullptr);
    setup.rig.window().setRecordingActive(true);
    Q_EMIT tab->setupRequested();
    CHECK(setup.setups.empty());
    CHECK(setup.rig.window().status() ==
          QStringLiteral("Finish Quick Record before checking meeting audio."));
    CHECK(tab->statusPill()->text() ==
          QStringLiteral("Finish Quick Record before checking meeting audio."));
    // The first start needs the same setup, so it does not start either.
    Q_EMIT tab->startRequested(false, QString());
    CHECK(setup.rig.controls().named("start").empty());
    setup.rig.window().setRecordingActive(false);
    Q_EMIT tab->setupRequested();
    CHECK(setup.setups == std::vector<bool>{false});
}

TEST_CASE("every Meeting Mode tab action reaches the runtime", "[widget]") {
    PresenterRig rig;
    auto* tab = rig.openMeetingTab();
    REQUIRE(tab != nullptr);
    auto& calls = rig.controls().calls;
    rig.controls().active = true;
    Q_EMIT tab->pauseRequested();
    Q_EMIT tab->resumeRequested();
    Q_EMIT tab->openDashboardRequested();
    Q_EMIT tab->openReportRequested();
    Q_EMIT tab->copyGuestLinkRequested();
    Q_EMIT tab->retryInsightsRequested();
    Q_EMIT tab->retrySpeakersRequested();
    Q_EMIT tab->retryStepRequested(QStringLiteral("polish"));
    Q_EMIT tab->backgroundRequested();
    Q_EMIT tab->deferInsightsRequested();
    Q_EMIT tab->pastMeetingRequested(QStringLiteral("m-1"));
    Q_EMIT rig.window().meetingDashboardRequested();
    Q_EMIT rig.window().pastMeetingRequested(QStringLiteral("m-2"));
    CHECK(calls == std::vector<std::string>{"pause", "resume", "open_dashboard", "open_report",
                                            "copy_guest_link", "retry_insights", "retry_speakers",
                                            "retry_step polish", "continue_in_background",
                                            "defer_finalization_card", "show_past_meeting m-1",
                                            "open_dashboard", "show_past_meeting m-2"});

    QString destination;
    QObject::connect(&rig.window(), &fv::MainWindow::settingsDestinationRequested,
                     [&destination](const QString& id) { destination = id; });
    Q_EMIT tab->aiSettingsRequested();
    CHECK(destination == QStringLiteral("meeting_intelligence"));

    calls.clear();
    std::unique_ptr<fv::SystemTray> tray;
    Q_EMIT trayOf(rig, tray).meetingDashboardRequested();
    CHECK(calls == std::vector<std::string>{"open_dashboard"});

    calls.clear();
    rig.controls().meeting_recovery_scan_failed.emit("The database is locked.");
    REQUIRE(QTest::qWaitFor([&] { return rig.window().findChild<QMessageBox*>() != nullptr; }));
    auto* warning = rig.window().findChild<QMessageBox*>();
    for (auto* button : warning->buttons()) {
        if (button->text() == QStringLiteral("Retry scan")) {
            button->click();
        }
    }
    REQUIRE(QTest::qWaitFor([&] { return !calls.empty(); }));
    CHECK(calls == std::vector<std::string>{"retry_recovery_scan"});
}

TEST_CASE("every Past Meetings action reaches the runtime", "[widget]") {
    PresenterRig rig;
    auto* panel = rig.window().findChild<fv::PastMeetingsPanel*>();
    REQUIRE(panel != nullptr);
    auto& calls = rig.controls().calls;
    Q_EMIT panel->meetingSelected(QStringLiteral("m-1"));
    Q_EMIT panel->deleteMeetingRequested(QStringLiteral("m-2"), true);
    Q_EMIT panel->deleteMeetingRequested(QStringLiteral("m-3"), false);
    Q_EMIT panel->clearMeetingsRequested(false);
    CHECK(calls ==
          std::vector<std::string>{"show_past_meeting m-1", "delete_past_meeting m-2|audio",
                                   "delete_past_meeting m-3|keep", "clear_past_meetings keep"});
}

TEST_CASE("the explicit speaker pass names what is sent to OpenAI and defaults to Cancel",
          "[widget]") {
    PresenterRig rig;
    auto* panel = rig.window().findChild<fv::PastMeetingsPanel*>();
    REQUIRE(panel != nullptr);
    Q_EMIT panel->speakerUploadRequested(QStringLiteral("m-1"));
    REQUIRE(rig.controls().calls == std::vector<std::string>{"request_speaker_upload m-1"});
    REQUIRE(rig.controls().speakerConfirm);
    fv::meeting::SpeakerUploadOffer offer;
    offer.meeting_id = "m-1";
    offer.title = "Weekly sync";
    offer.started_at = "2026-11-14 10:00";
    for (const bool send : {false, true}) {
        QString text;
        QString detail;
        bool cancelIsDefault = false;
        QTimer::singleShot(0, &rig.window(), [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            REQUIRE(box != nullptr);
            text = box->text();
            detail = box->informativeText();
            cancelIsDefault = box->defaultButton() == box->button(QMessageBox::Cancel);
            for (auto* button : box->buttons()) {
                if ((button->text() == QStringLiteral("Send system audio")) == send &&
                    (send || box->buttonRole(button) == QMessageBox::RejectRole)) {
                    button->click();
                    return;
                }
            }
        });
        CHECK(rig.controls().speakerConfirm(offer) == send);
        CHECK(text == QStringLiteral("Send system audio from “Weekly sync” to OpenAI?"));
        CHECK(detail.contains(QStringLiteral("system-audio track")));
        CHECK(detail.contains(QStringLiteral("microphone track, transcript, and participant "
                                             "names stay on this computer")));
        CHECK(cancelIsDefault);
    }
}

TEST_CASE("a copied guest link and a copied transcript say what was copied", "[widget]") {
    PresenterRig rig;
    auto* tab = rig.openMeetingTab();
    REQUIRE(tab != nullptr);
    rig.controls().meeting_guest_link_ready.emit("http://127.0.0.1:8765/guest?token=t");
    REQUIRE(QTest::qWaitFor(
        [&] { return rig.window().status() == QStringLiteral("Guest link copied"); }));
    CHECK(tab->statusPill()->text() == QStringLiteral("Guest link copied"));
    REQUIRE_FALSE(rig.clipboard().writes.empty());
    CHECK(rig.clipboard().writes.back().text ==
          QStringLiteral("http://127.0.0.1:8765/guest?token=t"));

    auto* panel = rig.window().findChild<fv::PastMeetingsPanel*>();
    REQUIRE(panel != nullptr);
    Q_EMIT panel->copyTranscriptRequested(QStringLiteral("m-1"));
    REQUIRE(QTest::qWaitFor(
        [&] { return rig.window().status() == QStringLiteral("Transcript copied"); }));
    CHECK(rig.clipboard().writes.back().text == QStringLiteral("Speaker 1: hello"));

    // Nothing to copy says nothing.
    rig.window().setStatus(QStringLiteral("Ready"));
    rig.controls().transcript.clear();
    Q_EMIT panel->copyTranscriptRequested(QStringLiteral("m-2"));
    QTest::qWait(100);
    CHECK(rig.window().status() == QStringLiteral("Ready"));
}

TEST_CASE("the state payload reaches the tab, the tabs, Past Meetings and the tray", "[widget]") {
    PresenterRig rig;
    auto* tab = rig.openMeetingTab();
    REQUIRE(tab != nullptr);
    auto* tabs = rig.window().tabbedContent();
    tabs->setCurrentIndex(fv::TabbedContentWidget::TabQuickRecord);
    std::unique_ptr<fv::SystemTray> tray;
    trayOf(rig, tray);
    auto* panel = rig.window().findChild<fv::PastMeetingsPanel*>();
    REQUIRE(panel != nullptr);
    panel->setSelectedMeetingId(QStringLiteral("m-0"));
    auto* dashboardAction = rig.context().menus().action(fv::menu_id::meetingDashboard);
    auto* trayMeeting = rig.context().menus().action(fv::tray_id::meeting);
    auto* trayDashboard = rig.context().menus().action(fv::tray_id::meetingDashboard);
    REQUIRE(trayMeeting != nullptr);
    REQUIRE(trayDashboard != nullptr);

    rig.window().setHostMode(true, false);
    rig.controls().meeting_state_changed.emit(
        Json{{"active", true}, {"status", "recording"}, {"meeting_id", "m-1"}});
    QApplication::processEvents();
    // A meeting starting here needs its tab, even on a host, and holds the other tabs.
    CHECK_FALSE(rig.window().isHostMode());
    CHECK(tabs->currentIndex() == fv::TabbedContentWidget::TabMeetingMode);
    CHECK_FALSE(tabs->tabBar()->isTabEnabled(fv::TabbedContentWidget::TabQuickRecord));
    CHECK(tab->isMeetingActive());
    CHECK(panel->selectedMeetingId().isEmpty());
    CHECK(trayMeeting->text() == QStringLiteral("End Meeting"));
    // No dashboard yet: the tab hears so though the payload did not say, and the guest link,
    // which needs one, stays off.
    CHECK_FALSE(tab->guestLinkButton()->isEnabled());

    rig.controls().meeting_server_started.emit(Json{{"host_url", "http://127.0.0.1:1/host"}});
    REQUIRE(QTest::qWaitFor([&] { return tab->guestLinkButton()->isEnabled(); }));
    CHECK(rig.controls().named("open_dashboard").size() == 1);
    REQUIRE(dashboardAction != nullptr);
    CHECK(dashboardAction->isEnabled());
    CHECK(trayDashboard->isEnabled());

    rig.controls().meeting_state_changed.emit(
        Json{{"active", false}, {"status", "ended"}, {"meeting_id", "m-1"}});
    QApplication::processEvents();
    CHECK(tabs->tabBar()->isTabEnabled(fv::TabbedContentWidget::TabQuickRecord));
    CHECK(panel->selectedMeetingId() == QStringLiteral("m-1"));
    CHECK(trayMeeting->text() == QStringLiteral("Start Meeting"));
    // Open Dashboard stays available while the ended meeting finalizes.
    CHECK(trayDashboard->isEnabled());
    CHECK(dashboardAction->isEnabled());

    rig.controls().meeting_state_changed.emit(Json{{"dashboard_available", false}});
    QApplication::processEvents();
    CHECK_FALSE(dashboardAction->isEnabled());
    CHECK_FALSE(trayDashboard->isEnabled());
}

TEST_CASE("a meeting error after a code change offers a restart with the error", "[widget]") {
    PresenterRig rig;
    QString offered;
    // Bootstrap provides the shell's lifecycle hooks; the controller fills the restart offer.
    auto shared = std::make_shared<fv::ShellLifecycle>();
    rig.context().provide(shared);
    auto* lifecycle = shared.get();
    bool changed = true;
    lifecycle->offerRestartForFailure = [&](const QString& failure) {
        offered = failure;
        return changed;
    };
    rig.controls().meeting_error.emit("Meeting engine failed to load.");
    REQUIRE(QTest::qWaitFor([&] { return !offered.isEmpty(); }));
    CHECK(offered == QStringLiteral("Meeting engine failed to load."));
    CHECK(rig.window().status() == QStringLiteral("Meeting engine failed to load."));
    QApplication::processEvents();
    CHECK(rig.window().findChild<QMessageBox*>() == nullptr);

    // Unchanged code: the plain warning.
    changed = false;
    rig.controls().meeting_error.emit("");
    REQUIRE(QTest::qWaitFor([&] { return rig.window().findChild<QMessageBox*>() != nullptr; }));
    auto* box = rig.window().findChild<QMessageBox*>();
    CHECK(box->windowTitle() == QStringLiteral("Meeting Error"));
    CHECK(box->text() == QStringLiteral("Meeting error"));
    box->close();
}

TEST_CASE("the tab's speech line names the remote host the meeting would use", "[widget]") {
    PresenterRig rig(Json{{"meeting_speech_separate", true}, {"meeting_asr_source", "remote"}});
    auto* tab = rig.openMeetingTab();
    REQUIRE(tab != nullptr);
    CHECK(tab->speechDestination()->text() ==
          QStringLiteral("Remote speech · microphone and system audio sent to a host (not paired "
                         "yet). Recordings saved here."));
    rig.settings().save_setting("meeting_asr_source", "local");
    QApplication::processEvents();
    CHECK(tab->speechDestination()->text().endsWith(
        QStringLiteral("Speech recognition runs on this computer.")));
}

TEST_CASE("the tab's AI line is the destination and what leaves this computer", "[widget]") {
    PresenterRig rig(Json{{"meeting_llm_provider", "openrouter"},
                          {"meeting_llm_model", "deepseek/deepseek-v4.1-flash"}});
    auto* tab = rig.openMeetingTab();
    REQUIRE(tab != nullptr);
    CHECK(tab->aiDestination()->text() ==
          QStringLiteral("OpenRouter · deepseek-v4.1-flash — sends transcript text, never audio."));
}

TEST_CASE("the empty stage lists the three newest finished meetings, not the live one",
          "[widget]") {
    PresenterRig rig;

    fvtest::offGui([&] {
        const auto meeting = [&](const char* id, const char* status, const char* started) {
            rig.repository().create_meeting({{"id", id},
                                             {"title", id},
                                             {"status", status},
                                             {"started_at", started},
                                             {"host_token", std::string("host-") + id},
                                             {"guest_token", std::string("guest-") + id},
                                             {"cloud_enabled", false},
                                             {"spool_dir", ""}});
        };
        meeting("m1", "ended", "2026-11-10T09:00:00Z");
        meeting("m2", "ended", "2026-11-11T09:00:00Z");
        meeting("m3", "ended", "2026-11-12T09:00:00Z");
        meeting("m4", "active", "2026-11-13T09:00:00Z");
    });
    rig.window().show();
    auto* tab = rig.openMeetingTab();
    REQUIRE(tab != nullptr);
    const auto shown = [tab] {
        std::vector<QString> ids;
        for (const auto* row : tab->recentRows()) {
            ids.push_back(row->meetingId());
        }
        return ids;
    };
    REQUIRE(QTest::qWaitFor([&] { return shown().size() == 3; }, 3000));
    CHECK(shown() ==
          std::vector<QString>{QStringLiteral("m3"), QStringLiteral("m2"), QStringLiteral("m1")});
}
