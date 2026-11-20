// The `api` entry of the app's speech engine table (application_controller.py
// _setup_transcription_backends): Bootstrap builds the cloud backend over its own network
// client, credential store, .env file and upload strand, behind the table its hook gives. No
// request is sent: a load only asks whether a key resolves.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>

#include "appkit/app_context.h"
#include "appkit/feature.h"
#include "audio/testing/fake_backend.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "credentials/testing/memory_backend.h"
#include "shell_fixture.h"
#include "speech/service.h"
#include "testkit/testkit.h"
#include "ui_shell/application.h"
#include "ui_shell/bootstrap.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_shell/single_instance.h"
#include "ui_shell/system_tray.h"

using namespace std::chrono_literals;

namespace {

// What the engine in the dictation slot said once its load ended, and whether it takes files.
struct Loaded {
    fv::speech::Readiness readiness{fv::speech::ReadyState::Loading, {}, {}};
    bool acceptsFiles = false;
    bool applicationProvided = false;
};

// Starts the app with the shell alone over `env` (the temporary data folder, a per-user folder
// inside it, and only the keys the test gives), selects the API engine for Groq in the
// dictation slot and waits for its load.
Loaded startWithApiEngine(fv::MapEnvReader& env) {
    fv::testkit::TempDataRoot root;
    const std::string dataDir = std::string(fv::identity().env_prefix) + "DATA_DIR";
    if (const std::optional<std::string> value = fv::system_env().get(dataDir)) {
        env.set(dataDir, *value);
    }
    const std::filesystem::path perUser = root.path() / "per-user";
    std::filesystem::create_directories(perUser);
    for (const char* name : {"LOCALAPPDATA", "XDG_DATA_HOME", "HOME"}) {
        env.set(name, fv::base_utf8::path_to_utf8(perUser));
    }

    fv::BootstrapHooks hooks;
    hooks.env = &env;
    hooks.features = [] {
        std::vector<std::unique_ptr<fv::Feature>> features;
        features.push_back(std::make_unique<fv::ShellFeature>());
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
    hooks.errorBox = [](const std::string&) {
    };
    hooks.arguments = std::vector<std::string>{"openwhisper-cpp"};
    hooks.audioBackend = [] {
        return std::make_shared<fv::audio::testing::FakeBackend>();
    };

    Loaded loaded;
    hooks.run = [&loaded](fv::Application&, fv::MainWindow& window) {
        fv::AppContext& context = window.context();
        loaded.applicationProvided = context.find<fv::Application>() != nullptr;
        fv::speech::SpeechService& speech = context.speech();
        fv::speech::BackendConfig config;
        config.backend = "api";
        config.settings = std::make_shared<const fv::Json>(
            fv::Json{{"selected_model", "api"}, {"api_transcription_endpoint", "groq"}});
        speech.select(fv::speech::Slot::Dictation, std::move(config));
        fvtest::waitUntil(
            [&speech] {
                const auto state = speech.readiness(fv::speech::Slot::Dictation).state;
                return state != fv::speech::ReadyState::Loading &&
                       state != fv::speech::ReadyState::Warming;
            },
            10s);
        loaded.readiness = speech.readiness(fv::speech::Slot::Dictation);
        loaded.acceptsFiles = speech.accepts_files(fv::speech::Slot::Dictation);
        return 0;
    };
    fv::Bootstrap bootstrap(std::move(hooks));
    int argc = 1;
    char program[] = "openwhisper-cpp";
    char* argv[] = {program, nullptr};
    CHECK(bootstrap.run(argc, argv) == 0);
    return loaded;
}

} // namespace

TEST_CASE("the app's API engine asks for a key when none resolves", "[widget]") {
    fv::MapEnvReader env;
    const Loaded loaded = startWithApiEngine(env);

    CHECK(loaded.readiness.state == fv::speech::ReadyState::NeedsKey);
    CHECK(loaded.readiness.device_info ==
          "The selected API transcription engine needs a valid API key");
    CHECK(loaded.applicationProvided);
}

TEST_CASE("the app's API engine is ready with a key and takes whole files", "[widget]") {
    fv::MapEnvReader env{{"GROQ_API_KEY", "gsk-test"}};
    const Loaded loaded = startWithApiEngine(env);

    CHECK(loaded.readiness.state == fv::speech::ReadyState::Ready);
    CHECK(loaded.readiness.device_info.starts_with("Groq ("));
    CHECK(loaded.acceptsFiles);
}
