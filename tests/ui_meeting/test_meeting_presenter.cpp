#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

#include <QApplication>
#include <QDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QTabBar>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include "appcore/activity.h"
#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "audio/testing/fake_backend.h"
#include "db/database.h"
#include "meeting/runtime.h"
#include "meeting_agent/testing/fake_agent_core.h"
#include "meeting_state/sql_repository.h"
#include "net/reactor.h"
#include "net/testing/ws_test_client.h"
#include "past_meetings_support.h"
#include "settings/testing/memory_store.h"
#include "speech/testing/fake_backend.h"
#include "testkit/testkit.h"
#include "ui_meeting/ai_insights_copy.h"
#include "ui_meeting/meeting_mode_tab.h"
#include "ui_meeting/meeting_presenter.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_shell/system_tray.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/tabbed_content.h"

namespace {

using namespace std::chrono_literals;

class FakeLiveIntelligence final : public fv::meeting::IntelligenceProvider {
public:
    explicit FakeLiveIntelligence(std::shared_ptr<fv::MeetingConsentGrants> grants)
        : grants_(std::move(grants)) {}

    bool available() const override { return true; }
    std::optional<fv::meeting_agent::MeetingAgentRoute> live_destination() const override {
        return route;
    }
    bool authorize_live_destination(
        const fv::meeting_agent::MeetingAgentRoute& approved) const override {
        return approved == route && grants_->allows(approved);
    }
    std::shared_ptr<fv::meeting_agent::AgentCore>
    create_live_core(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
        return std::make_shared<fv::meeting_agent::testing::FakeAgentCore>();
    }
    fv::Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
        return {{"status", "completed"}};
    }
    fv::Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
        return {{"status", "completed"}};
    }
    fv::meeting_agent::MeetingAgentRoute route;

private:
    std::shared_ptr<fv::MeetingConsentGrants> grants_;
};

TEST_CASE("Meeting Mode starts and ends through the desktop presenter", "[widget]") {
    fv::testkit::TempDataRoot folder;
    fv::MemorySettingsStore settings(fv::Json{{"meeting_setup_complete", true},
                                              {"meeting_mode_intro_seen", true},
                                              {"meeting_unsupported_platform_ack", true},
                                              {"meeting_speaker_id_backend", "off"}});
    fv::qt::QtUiExecutor ui(qApp);
    fv::ThreadPool io("meeting-test", 1);
    fv::net::Reactor reactor;
    fv::ActivityState activity;
    auto audio = std::make_shared<fv::audio::testing::FakeBackend>();
    audio->set_layout(
        {{.name = "MME", .default_input_device = 0}},
        {{.name = "Mic 0", .hostapi = 0, .max_input_channels = 1, .default_samplerate = 16000}}, 0);
    audio->set_loopbacks({{.id = "{a}", .name = "Speakers {a}", .is_default = true}});
    auto fakeSpeech = std::make_shared<fv::speech::FakeEngine>(fv::speech::FakeScript{});
    fv::speech::SpeechService speech(fakeSpeech->factory(), ui);
    fv::speech::BackendConfig config;
    config.backend = "parakeet";
    config.model_key = "parakeet-v3";
    speech.select(fv::speech::Slot::Dictation, config);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return speech.readiness(fv::speech::Slot::Meeting).state ==
                   fv::speech::ReadyState::Ready;
        },
        5000));

    auto database =
        fvtest::offGui([&] { return fv::Database::open_or_create(folder.path() / "meetings.db"); });
    auto repository = std::make_shared<fv::meeting_state::SqlMeetingRepository>(database);
    fv::meeting::MeetingRuntimeDeps deps;
    deps.ui = &ui;
    deps.settings = &settings;
    deps.repository = repository;
    deps.audio = audio;
    deps.speech = &speech;
    deps.reactor = &reactor;
    deps.activity = &activity;
    deps.meetings_root = folder.path() / "meetings";
    const auto grants = std::make_shared<fv::MeetingConsentGrants>();
    auto intelligence = std::make_shared<FakeLiveIntelligence>(grants);
    intelligence->route = fv::meeting_agent::resolve_meeting_agent_route(*settings.snapshot());
    deps.intelligence = intelligence;
    QUrl opened;
    deps.open_url = [&](std::string_view url) {
        opened = QUrl(fv::qt::toQString(url));
        return true;
    };
    {
        fv::meeting::MeetingRuntime runtime(std::move(deps));
        fv::AppServices services;
        services.settings = &settings;
        services.ui = &ui;
        services.io = &io;
        services.activity = &activity;
        services.audio = audio.get();
        services.clock = &fv::system_clock();
        fv::AppContext context(services);
        fv::ShellFeature shell;
        shell.provide(context);
        shell.install(context);
        auto& window = context.service<fv::MainWindow>();
        auto* presenter = new fv::MeetingPresenter(context, window, runtime, {}, repository,
                                                   folder.path(), grants);
        presenter->install();
        window.completeSetup();
        auto* tabs = window.tabbedContent();
        const int index = fv::TabbedContentWidget::TabMeetingMode;
        CHECK(tabs->tabBar()->tabText(index) == QStringLiteral("Meeting Mode"));
        CHECK(tabs->tabBar()->count() == index + 1);
        tabs->setCurrentIndex(index);
        auto* tab = qobject_cast<fv::MeetingModeTab*>(window.page(index));
        REQUIRE(tab);
        window.resize(390, 520);
        const auto failScan = [&](const std::string& error) {
            runtime.meeting_recovery_scan_failed.emit(error);
            return QTest::qWaitFor(
                [&] { return window.status() == QStringLiteral("Meeting recovery scan failed"); });
        };
        REQUIRE(failScan("Saved meetings could not be checked."));
        auto* warning = window.findChild<QMessageBox*>();
        REQUIRE(warning);
        REQUIRE(warning->isVisible());
        CHECK_FALSE(warning->isModal());
        REQUIRE(failScan("The database is still unavailable."));
        REQUIRE(QTest::qWaitFor([&] { return warning->text().contains(QStringLiteral("still")); }));
        REQUIRE(window.findChildren<QMessageBox*>().size() == 1);
        auto* retry = warning->button(QMessageBox::Retry);
        for (auto* button : warning->buttons()) {
            if (button->text() == QStringLiteral("Retry scan")) {
                retry = button;
            }
        }
        REQUIRE(retry);
        retry->click();
        REQUIRE(QTest::qWaitFor([&] { return window.findChildren<QMessageBox*>().empty(); }));
        tab->startButton()->click();
        REQUIRE(QTest::qWaitFor([&] { return tab->isMeetingActive() && opened.isValid(); }, 8000));
        CHECK(activity.snapshot().meeting_active);
        CHECK_FALSE(tabs->tabBar()->isTabEnabled(fv::TabbedContentWidget::TabQuickRecord));

        // Extract the token from the live meeting row without opening a browser.
        const auto rows = fvtest::offGui([&] {
            fv::meeting_state::PastMeetingQuery query;
            query.include_running = true;
            return repository->list_past_meeting_summaries(query);
        });
        REQUIRE(rows.size() == 1);
        const auto id = rows.front().at("id").get<std::string>();
        const auto meeting = fvtest::offGui([&] { return repository->get_meeting(id); });
        REQUIRE(meeting);
        fv::net::testing::WsTestClient socket;
        const auto port = static_cast<std::uint16_t>(opened.port());
        REQUIRE(socket.connect(port, "/ws?token=" + meeting->at("host_token").get<std::string>()));
        const auto hello = socket.receive(3s);
        REQUIRE(hello);
        CHECK(fv::Json::parse(hello->data).at("type") == "hello");

        fv::net::testing::WsTestClient guest;
        REQUIRE(guest.connect(port, "/ws?token=" + meeting->at("guest_token").get<std::string>() +
                                        "&name=Reviewer"));
        const auto guestHello = guest.receive(3s);
        REQUIRE(guestHello);
        CHECK(fv::Json::parse(guestHello->data).at("type") == "hello");

        auto* insights = tab->cloudCheckbox();
        REQUIRE(insights);
        REQUIRE_FALSE(insights->isChecked());
        const auto answer = [&](bool accepted) {
            QTimer::singleShot(0, &window, [accepted] {
                if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                    dialog->done(accepted ? QDialog::Accepted : QDialog::Rejected);
                }
            });
        };
        answer(false);
        insights->click();
        CHECK_FALSE(insights->isChecked());
        CHECK_FALSE(grants->allows(intelligence->route));

        answer(true);
        insights->click();
        REQUIRE(QTest::qWaitFor([&] { return insights->isChecked(); }, 4000));
        CHECK(grants->allows(intelligence->route));
        insights->click();
        REQUIRE(QTest::qWaitFor([&] { return !insights->isChecked(); }, 4000));

        // Even when the grant store remembers the same destination, a fresh live enable asks.
        answer(false);
        insights->click();
        CHECK_FALSE(insights->isChecked());
        answer(true);
        insights->click();
        REQUIRE(QTest::qWaitFor([&] { return insights->isChecked(); }, 4000));

        QTimer::singleShot(0, &window, [] {
            if (auto* question = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                if (auto* yes = question->button(QMessageBox::Yes)) {
                    yes->click();
                }
            }
        });
        tab->endButton()->click();
        REQUIRE(QTest::qWaitFor([&] { return !tab->isMeetingActive(); }, 8000));
        CHECK(tabs->tabBar()->isTabEnabled(fv::TabbedContentWidget::TabQuickRecord));
        CHECK_FALSE(activity.snapshot().meeting_active);
        guest.abort();
        socket.abort();
        runtime.cleanup();
        io.shutdown(2000ms);
    }
    fvtest::offGui([&] { database.close(); });
}

} // namespace
