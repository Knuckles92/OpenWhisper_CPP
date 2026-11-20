// ui_qt/ui_controller.py switch_to_tab / switch_to_meeting_mode: a meeting that starts while the
// window shows Host Mode's dashboard leaves Host Mode for the Meeting Mode tab.
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QLabel>
#include <QTest>

#include "appcore/activity.h"
#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "audio/testing/fake_backend.h"
#include "db/database.h"
#include "meeting/runtime.h"
#include "meeting_state/sql_repository.h"
#include "past_meetings_support.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "speech/testing/fake_backend.h"
#include "testkit/testkit.h"
#include "ui_meeting/meeting_presenter.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_widgets/tabbed_content.h"

TEST_CASE("a meeting that starts in Host Mode switches to its tab", "[widget]") {
    fv::testkit::TempDataRoot folder;
    fv::MemorySettingsStore settings(fv::Json{{"meeting_setup_complete", true},
                                              {"meeting_mode_intro_seen", true},
                                              {"meeting_unsupported_platform_ack", true}});
    fv::qt::QtUiExecutor ui(qApp);
    fv::ThreadPool io("meeting-host-mode", 1);
    fv::ActivityState activity;
    auto fakeSpeech = std::make_shared<fv::speech::FakeEngine>(fv::speech::FakeScript{});
    fv::speech::SpeechService speech(fakeSpeech->factory(), ui);
    auto database =
        fvtest::offGui([&] { return fv::Database::open_or_create(folder.path() / "meetings.db"); });
    auto repository = std::make_shared<fv::meeting_state::SqlMeetingRepository>(database);
    fv::meeting::MeetingRuntimeDeps deps;
    deps.ui = &ui;
    deps.settings = &settings;
    deps.repository = repository;
    deps.speech = &speech;
    deps.activity = &activity;
    deps.meetings_root = folder.path() / "meetings";
    {
        fv::meeting::MeetingRuntime runtime(std::move(deps));
        fv::AppServices services;
        services.settings = &settings;
        services.ui = &ui;
        services.io = &io;
        services.activity = &activity;
        services.clock = &fv::system_clock();
        fv::AppContext context(services);
        fv::ShellFeature shell;
        shell.provide(context);
        shell.install(context);
        auto& window = context.service<fv::MainWindow>();
        auto* presenter =
            new fv::MeetingPresenter(context, window, runtime, {}, repository, folder.path());
        presenter->install();
        window.completeSetup();
        window.setHostDashboardFactory(
            [](QWidget* parent) -> QWidget* { return new QLabel("Host", parent); });
        window.setHostMode(true);
        REQUIRE(window.isHostMode());

        runtime.meeting_state_changed.emit(fv::Json{{"active", true}});
        REQUIRE(QTest::qWaitFor([&] { return !window.isHostMode(); }, 5000));
        CHECK(window.tabbedContent()->currentIndex() == fv::TabbedContentWidget::TabMeetingMode);
        // The choice is saved, as Python's set_host_mode(False) does.
        CHECK_FALSE(settings.get(fv::keys::host_mode));

        // An ending meeting does not bring Host Mode back.
        runtime.meeting_state_changed.emit(fv::Json{{"active", false}});
        QApplication::processEvents();
        CHECK_FALSE(window.isHostMode());
        runtime.cleanup();
    }
}
