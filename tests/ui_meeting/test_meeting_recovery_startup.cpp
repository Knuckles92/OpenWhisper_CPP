// When the startup recovery scan for interrupted meetings runs (MW-12): OpenWhisper calls
// meeting_runtime.setup() from ApplicationController._on_startup_database_ready, which follows
// notify_main_ui_ready, so the scan and its recovery dialog never come up behind the splash. Here
// the app starts through Bootstrap with MeetingFeature and a stand-in for the controller that
// marks the startup database ready (ActivityState::startup_ready, as
// ApplicationController::on_startup_database_ready does) when the main window is shown.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <QApplication>
#include <QSystemTrayIcon>
#include <QTest>

#include "appcore/activity.h"
#include "appkit/app_context.h"
#include "appkit/feature.h"
#include "audio/testing/fake_backend.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/paths.h"
#include "credentials/testing/memory_backend.h"
#include "db/database.h"
#include "meeting/runtime.h"
#include "meeting_state/sql_repository.h"
#include "models/component_store.h"
#include "models/coordinator.h"
#include "models/hf_access.h"
#include "models/speech_catalog.h"
#include "models/speech_model_cache.h"
#include "past_meetings_support.h"
#include "testkit/testkit.h"
#include "ui_history/history_feature.h"
#include "ui_meeting/meeting_feature.h"
#include "ui_settings/settings_feature.h"
#include "ui_shell/application.h"
#include "ui_shell/bootstrap.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_shell/single_instance.h"

using namespace std::chrono_literals;

namespace {

// What happened during startup, in order: "shown" when the main window was shown, "found"
// when the recovery scan reported the interrupted meeting.
struct Events {
    std::vector<std::string> order;
    bool databaseReadyAtShow = true;
};

// The component store and its coordinator, released together.
struct Components {
    Components(const std::filesystem::path& root, fv::http::Client& client)
        : store(root, client), coordinator(store) {}
    fv::models::ComponentStore store;
    fv::models::ComponentCoordinator coordinator;
};

// Stands in for ControllerFeature: provides the lifecycle; when the main window is shown it
// opens the startup database (sets startup_ready) unless the test holds it back.
class ControllerStandIn final : public fv::Feature {
public:
    explicit ControllerStandIn(Events& events) : events_(events) {}
    std::string_view id() const override { return "controller"; }
    std::vector<std::string_view> required_features() const override { return {"shell"}; }
    void provide(fv::AppContext& context) override {
        // The model services the controller provides and MeetingFeature's setup check reads.
        const fv::Paths& paths = context.paths();
        auto cache = std::make_shared<fv::models::SpeechModelCache>(paths.speech_models_root(),
                                                                    context.http());
        auto components = std::make_shared<Components>(paths.components_root(), context.http());
        context.provide(cache);
        context.provide(std::shared_ptr<fv::models::ComponentCoordinator>(
            components, &components->coordinator));
        context.provide(std::make_shared<fv::models::HuggingFaceAccessCoordinator>(
            context.settings(), [cache](std::string_view name) {
                return fv::models::model(name) != nullptr && cache->is_cached(name);
            }));
        auto lifecycle = std::make_shared<fv::ShellLifecycle>();
        fv::ActivityState* activity = &context.activity();
        lifecycle->mainWindowShown = [this, activity] {
            events_.order.push_back("shown");
            if (events_.databaseReadyAtShow) {
                activity->set_startup_ready(true);
            }
        };
        context.provide(lifecycle);
    }
    void install(fv::AppContext& context) override {
        auto* runtime = context.find<fv::meeting::MeetingRuntime>();
        REQUIRE(runtime != nullptr);
        subscription_ = runtime->meeting_recovery_found.connect(
            context.ui(), [this](std::vector<fv::Json>) { events_.order.push_back("found"); });
    }

private:
    Events& events_;
    fv::Subscription subscription_;
};

// A meeting left recording by a process that is gone: the scan offers it for recovery.
void seedInterruptedMeeting() {
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    const std::filesystem::path file = paths.user_file(fv::identity().database_file);
    std::filesystem::create_directories(file.parent_path());
    fvtest::offGui([&] {
        auto database = fv::Database::open_or_create(file);
        fv::meeting_state::SqlMeetingRepository repository(database);
        repository.create_meeting({{"id", "m-interrupted"},
                                   {"title", "Interrupted"},
                                   {"status", "active"},
                                   {"started_at", "2026-11-18T09:00:00Z"},
                                   {"host_token", "host-m"},
                                   {"guest_token", "guest-m"},
                                   {"cloud_enabled", false},
                                   {"spool_dir", ""}});
        database.close();
    });
}

// Starts the app; `run` stands for the event loop after the window was shown.
void startUp(const fv::testkit::TempDataRoot& root, Events& events,
             const std::function<void()>& run) {
    fv::BootstrapHooks hooks;
    hooks.features = [&events] {
        std::vector<std::unique_ptr<fv::Feature>> features;
        features.push_back(std::make_unique<fv::ShellFeature>());
        features.push_back(std::make_unique<fv::HistoryFeature>());
        features.push_back(std::make_unique<fv::SettingsFeature>());
        features.push_back(std::make_unique<ControllerStandIn>(events));
        features.push_back(std::make_unique<fv::MeetingFeature>());
        return features;
    };
    hooks.setupLogging = [](const fv::Paths&) {
        return std::shared_ptr<void>();
    };
    const std::filesystem::path locks = root.path() / "run";
    hooks.acquireInstance = [locks](const fv::Paths& paths) {
        fv::SingleInstanceOptions options;
        options.lockFolder = locks;
        options.handoff = 200ms;
        return fv::SingleInstance::acquire(paths.data_root(), fv::identity(), options);
    };
    hooks.credentialBackend = [] {
        return std::make_shared<fv::MemoryCredentialBackend>();
    };
    hooks.tray.available = [] {
        return true;
    };
    hooks.tray.showIcon = [](QSystemTrayIcon&) {
    };
    // The splash's event-loop turns: give a scan started behind the splash time to report.
    hooks.processEvents = [] {
        QApplication::processEvents();
    };
    hooks.run = [&run](fv::Application&, fv::MainWindow&) {
        run();
        return 0;
    };
    std::vector<std::string> errors;
    hooks.errorBox = [&errors](const std::string& message) {
        errors.push_back(message);
    };
    hooks.arguments = std::vector<std::string>{"fiorivoce"};
    hooks.audioBackend = [] {
        return std::make_shared<fv::audio::testing::FakeBackend>();
    };
    int argc = 1;
    char program[] = "fiorivoce";
    char* argv[] = {program, nullptr};
    fv::Bootstrap bootstrap(std::move(hooks));
    REQUIRE(bootstrap.run(argc, argv) == 0);
    CHECK(errors.empty());
}

} // namespace

TEST_CASE("the meeting recovery scan runs once the startup database is ready, after the main "
          "window is shown",
          "[MW-12][widget]") {
    fv::testkit::TempDataRoot root;
    seedInterruptedMeeting();
    Events events;
    startUp(root, events, [&events] {
        REQUIRE(QTest::qWaitFor([&events] { return events.order.size() == 2; }, 5000));
    });
    CHECK(events.order == std::vector<std::string>{"shown", "found"});
}

TEST_CASE("no meeting recovery scan runs while the startup database is not ready",
          "[MW-12][widget]") {
    fv::testkit::TempDataRoot root;
    seedInterruptedMeeting();
    Events events;
    events.databaseReadyAtShow = false;
    startUp(root, events, [&events] { QTest::qWait(500); });
    // The interrupted meeting is still there, but nothing scanned for it.
    CHECK(events.order == std::vector<std::string>{"shown"});
}
