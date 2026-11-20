// keep_separate_meeting_speech at startup (ui_qt/bootstrap.py:309-314): after the guided setup
// state and keep_unchosen_engine, before the main window, an existing install whose meetings ran
// on another engine than dictation keeps it as a separate engine, and every other install
// follows dictation. In OpenWhisper_CPP MeetingFeature::provide makes that call; here it runs
// through Bootstrap with the features it needs, and a stand-in for the controller (which provides
// the lifecycle) reads the settings when the main window is shown.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QApplication>
#include <QSystemTrayIcon>

#include "appkit/app_context.h"
#include "appkit/feature.h"
#include "audio/testing/fake_backend.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/paths.h"
#include "credentials/testing/memory_backend.h"
#include "settings/store.h"
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

constexpr std::string_view kSeparate = "meeting_speech_separate";

// Stands in for ControllerFeature: provides the lifecycle, and reads the saved choice when the
// main window is shown.
class ControllerStandIn final : public fv::Feature {
public:
    explicit ControllerStandIn(std::optional<fv::Json>& atShown) : atShown_(atShown) {}
    std::string_view id() const override { return "controller"; }
    std::vector<std::string_view> required_features() const override { return {"shell"}; }
    void provide(fv::AppContext& context) override {
        auto lifecycle = std::make_shared<fv::ShellLifecycle>();
        fv::SettingsStore* settings = &context.settings();
        lifecycle->mainWindowShown = [this, settings] {
            const fv::Json snapshot = *settings->snapshot();
            atShown_ = snapshot.contains(kSeparate) ? snapshot.at(kSeparate) : fv::Json();
        };
        context.provide(lifecycle);
    }
    void install(fv::AppContext&) override {}

private:
    std::optional<fv::Json>& atShown_;
};

// Runs the app's startup with `meeting` deciding whether MeetingFeature is linked; returns the
// saved choice as the main window was shown (null when none was saved).
fv::Json startUp(const fv::testkit::TempDataRoot& root, bool meeting) {
    std::optional<fv::Json> atShown;
    fv::BootstrapHooks hooks;
    hooks.features = [&atShown, meeting] {
        std::vector<std::unique_ptr<fv::Feature>> features;
        features.push_back(std::make_unique<fv::ShellFeature>());
        features.push_back(std::make_unique<fv::HistoryFeature>());
        features.push_back(std::make_unique<fv::SettingsFeature>());
        features.push_back(std::make_unique<ControllerStandIn>(atShown));
        if (meeting) {
            features.push_back(std::make_unique<fv::MeetingFeature>());
        }
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
    hooks.processEvents = [] {
        QApplication::processEvents();
    };
    hooks.run = [](fv::Application&, fv::MainWindow&) {
        return 0;
    };
    std::vector<std::string> errors;
    hooks.errorBox = [&errors](const std::string& message) {
        errors.push_back(message);
    };
    hooks.arguments = std::vector<std::string>{"openwhisper-cpp"};
    // No audio system is opened by a startup test.
    hooks.audioBackend = [] {
        return std::make_shared<fv::audio::testing::FakeBackend>();
    };
    int argc = 1;
    char program[] = "openwhisper-cpp";
    char* argv[] = {program, nullptr};
    fv::Bootstrap bootstrap(std::move(hooks));
    const int code = bootstrap.run(argc, argv);
    CHECK(errors.empty());
    REQUIRE(code == 0);
    REQUIRE(atShown.has_value());
    return *atShown;
}

// The settings file of the data folder TempDataRoot made: an existing install.
void writeSettings(const fv::Json& settings) {
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    const std::filesystem::path file = paths.user_file(fv::identity().settings_file);
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << settings.dump();
}

} // namespace

TEST_CASE("startup keeps an existing install's own meeting engine before the window shows",
          "[test_meeting_speech_follows_dictation][widget]") {
    fv::testkit::TempDataRoot root;
    // Dictation here, meetings on the paired computer.
    writeSettings(fv::Json{{"selected_model", "parakeet"}, {"meeting_asr_source", "remote"}});
    CHECK(startUp(root, true) == fv::Json(true));
}

TEST_CASE("startup makes a new install's meetings follow dictation before the window shows",
          "[test_meeting_speech_follows_dictation][widget]") {
    fv::testkit::TempDataRoot root;
    CHECK(startUp(root, true) == fv::Json(false));
}

TEST_CASE("the meeting engine choice at startup is MeetingFeature's",
          "[test_meeting_speech_follows_dictation][widget]") {
    fv::testkit::TempDataRoot root;
    writeSettings(fv::Json{{"selected_model", "parakeet"}, {"meeting_asr_source", "remote"}});
    // Without Meeting Mode linked nothing decides it.
    CHECK(startUp(root, false).is_null());
}
