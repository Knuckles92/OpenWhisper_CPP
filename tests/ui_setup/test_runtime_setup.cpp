// The runtime setup dialog and RuntimeSetup::ensureEngine. Adoption and picking run against
// OpenWhisper-style trees built in a temporary folder; the file pickers are behind the ports, so
// nothing here opens a file dialog, reads the owner's folders or touches the network.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <QObject>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "appkit/app_context.h"
#include "appkit/feature.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/testing/map_env_reader.h"
#include "models/catalog.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "setup_support.h"
#include "ui_setup/runtime_setup.h"
#include "ui_setup/runtime_setup_dialog.h"
#include "ui_setup/setup_feature.h"
#include "ui_widgets/animated_progress_bar.h"
#include "ui_widgets/buttons.h"

using namespace fvtest;
using Catch::Matchers::ContainsSubstring;

namespace {

std::shared_ptr<const fv::RuntimeSetupPorts> shared(fv::RuntimeSetupPorts ports) {
    return std::make_shared<const fv::RuntimeSetupPorts>(std::move(ports));
}

QString text(const std::string& utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

constexpr const char* kUnknownModel =
    "This file isn't a model OpenWhisper_CPP knows. Pick Parakeet TDT 0.6B v3 or Orukeet TDT 0.6B.";

} // namespace

TEST_CASE("the dialog lists an OpenWhisper folder's runtime and model and adopts them",
          "[ui_setup][widget]") {
    Fixture fx;
    fx.openwhisperRuntime();
    fx.openwhisperModel();
    const auto before = snapshotOf(fx.ow);

    fv::RuntimeSetupDialog dialog(shared(fx.ports()));
    // Nothing has been looked at yet: the look runs on the io executor.
    CHECK_FALSE(dialog.looked());
    CHECK_FALSE(dialog.useButton()->isEnabled());
    CHECK(dialog.readinessText() == QStringLiteral("Looking for the speech runtime and model…"));

    fx.settle();
    REQUIRE(dialog.looked());
    CHECK(dialog.offered() == QStringList{QStringLiteral("NVIDIA Speech CPU runtime"),
                                          QStringLiteral("Parakeet TDT 0.6B v3 model (16 B)")});
    CHECK(dialog.useButton()->isEnabled());
    CHECK(dialog.useButton()->text() == QStringLiteral("Use these"));
    CHECK(dialog.useButton()->objectName() == QStringLiteral("primaryButton"));
    CHECK(dialog.readinessText() ==
          QStringLiteral("Speech runtime: Missing\nSpeech model (Parakeet TDT 0.6B v3): Missing"));
    CHECK_FALSE(dialog.usable());
    CHECK(dialog.progressBar()->isHidden());

    dialog.useButton()->click();
    CHECK(dialog.busy());
    CHECK(dialog.progressBar()->isIndeterminate());
    CHECK_FALSE(dialog.progressBar()->isHidden());
    CHECK_FALSE(dialog.useButton()->isEnabled());
    CHECK_FALSE(dialog.folderButton()->isEnabled());
    CHECK_FALSE(dialog.modelButton()->isEnabled());
    CHECK(dialog.laterButton()->isEnabled());

    fx.settle();
    CHECK_FALSE(dialog.busy());
    CHECK(dialog.usable());
    CHECK(dialog.result() == QDialog::Accepted);
    // The OpenWhisper tree is its Windows build's (openwhisperRuntime writes win_amd64), so the
    // library linked into place is that build's DLL whatever this machine names its own.
    CHECK(std::filesystem::hard_link_count(fx.components / "asr-nvidia-cpu" / "bin" /
                                           "nemo_speech_asr_c.dll") == 2);
    CHECK(std::filesystem::hard_link_count(fx.speechModels / "parakeet-v3" / "parakeet.gguf") == 2);
    CHECK(snapshotOf(fx.ow) == before);
}

TEST_CASE("a model adopted without a runtime reports the model ready and leaves setup open",
          "[ui_setup][widget]") {
    Fixture fx;
    fx.openwhisperModel();

    fv::RuntimeSetupDialog dialog(shared(fx.ports()));
    fx.settle();
    REQUIRE(dialog.useButton()->isEnabled());
    dialog.useButton()->click();
    fx.settle();

    CHECK_FALSE(dialog.usable());
    CHECK(dialog.result() != QDialog::Accepted);
    CHECK(dialog.readinessText() ==
          QStringLiteral("Speech runtime: Missing\nSpeech model (Parakeet TDT 0.6B v3): Ready"));
    CHECK_THAT(dialog.resultText().toStdString(), ContainsSubstring("runtime in Downloads"));
}

TEST_CASE("a runtime adopted without a model leaves the dialog open and says what is missing",
          "[ui_setup][widget]") {
    Fixture fx;
    fx.openwhisperRuntime();

    fv::RuntimeSetupDialog dialog(shared(fx.ports()));
    fx.settle();
    CHECK(dialog.offered() == QStringList{QStringLiteral("NVIDIA Speech CPU runtime")});

    dialog.useButton()->click();
    fx.settle();
    CHECK(dialog.result() != QDialog::Accepted);
    CHECK_FALSE(dialog.usable());
    CHECK(dialog.resultText() ==
          QStringLiteral("Adopted NVIDIA Speech CPU. Download Parakeet TDT 0.6B v3 in Downloads."));
    CHECK(dialog.readinessText() ==
          QStringLiteral("Speech runtime: Ready\nSpeech model (Parakeet TDT 0.6B v3): Missing"));
    // It was adopted, so it is no longer offered; the buttons are back.
    CHECK(dialog.offered().isEmpty());
    CHECK_FALSE(dialog.useButton()->isEnabled());
    CHECK(dialog.folderButton()->isEnabled());
    CHECK(dialog.modelButton()->isEnabled());
    CHECK_FALSE(dialog.progressBar()->isIndeterminate());
    CHECK(dialog.progressBar()->isHidden());
}

TEST_CASE("an adoption that fails names what failed and keeps what worked", "[ui_setup][widget]") {
    Fixture fx;
    fx.openwhisperRuntime();
    // The size matches the pin, so it is offered; its hash does not, so adopting it is refused.
    fx.openwhisperModel("parakeet weighTs");

    fv::RuntimeSetupDialog dialog(shared(fx.ports()));
    fx.settle();
    REQUIRE(dialog.offered().size() == 2);

    dialog.useButton()->click();
    fx.settle();
    CHECK(dialog.result() != QDialog::Accepted);
    CHECK_THAT(dialog.resultText().toStdString(),
               ContainsSubstring("Couldn't adopt Parakeet TDT 0.6B v3: "));
    CHECK_FALSE(dialog.resultText().contains(QStringLiteral("Adopted")));
    CHECK(std::filesystem::exists(fx.components / "asr-nvidia-cpu" / ".installed"));
    CHECK_FALSE(std::filesystem::exists(fx.speechModels / "parakeet-v3"));
    CHECK(dialog.folderButton()->isEnabled());
}

TEST_CASE("with nothing to adopt the dialog says so and leaves Use these off",
          "[ui_setup][widget]") {
    Fixture fx;
    fv::RuntimeSetupDialog dialog(shared(fx.ports()));
    fx.settle();
    CHECK(dialog.offered().isEmpty());
    CHECK_FALSE(dialog.useButton()->isEnabled());
    CHECK(dialog.folderButton()->isEnabled());
    CHECK(dialog.modelButton()->isEnabled());
    CHECK(dialog.readinessText() ==
          QStringLiteral("Speech runtime: Missing\nSpeech model (Parakeet TDT 0.6B v3): Missing"));
}

TEST_CASE("a picked folder without the runtime library is refused with a message",
          "[ui_setup][widget]") {
    Fixture fx;
    const auto empty = fx.temp.path() / "empty";
    std::filesystem::create_directories(empty);
    fx.nextFolder = empty;

    fv::RuntimeSetupDialog dialog(shared(fx.ports()));
    fx.settle();
    dialog.folderButton()->click();
    fx.settle();

    CHECK(fx.folderPicks == 1);
    CHECK(fx.saves == 0);
    CHECK_THAT(dialog.resultText().toStdString(), ContainsSubstring("No speech runtime in "));
    CHECK_THAT(dialog.resultText().toStdString(), ContainsSubstring(empty.string()));
    CHECK_THAT(dialog.resultText().toStdString(), ContainsSubstring(fv::runtime_library_name()));
    CHECK(dialog.result() != QDialog::Accepted);
    CHECK(dialog.folderButton()->isEnabled());
}

TEST_CASE("a picked runtime folder is saved and, with a model in place, finishes the setup",
          "[ui_setup][widget]") {
    Fixture fx;
    fx.openwhisperModel();
    fx.adopter().adopt_model("parakeet-v3");
    const auto folder = fx.runtimeFolder();
    fx.nextFolder = folder;

    fv::RuntimeSetupDialog dialog(shared(fx.ports()));
    fx.settle();
    dialog.folderButton()->click();
    fx.settle();

    CHECK(fx.saves == 1);
    CHECK(fx.savedRuntimeDir == folder);
    CHECK(dialog.usable());
    CHECK(dialog.result() == QDialog::Accepted);
    CHECK_THAT(
        dialog.resultText().toStdString(),
        ContainsSubstring("Using the speech runtime in " + text(folder.string()).toStdString()));
}

TEST_CASE("a runtime folder chosen after a runtime was loaded asks for a restart",
          "[ui_setup][widget]") {
    Fixture fx;
    fx.runtimeLoaded = true;
    fx.nextFolder = fx.runtimeFolder();

    fv::RuntimeSetupDialog dialog(shared(fx.ports()));
    fx.settle();
    dialog.folderButton()->click();
    fx.settle();

    CHECK(fx.saves == 1);
    CHECK_THAT(dialog.resultText().toStdString(),
               ContainsSubstring("Restart OpenWhisper_CPP to switch to it."));
}

TEST_CASE("a cancelled picker changes nothing", "[ui_setup][widget]") {
    Fixture fx;
    fv::RuntimeSetupDialog dialog(shared(fx.ports()));
    fx.settle();

    dialog.folderButton()->click();
    dialog.modelButton()->click();
    CHECK(fx.folderPicks == 1);
    CHECK(fx.modelPicks == 1);
    CHECK_FALSE(dialog.busy());
    CHECK(fx.io.queued() == 0);
    CHECK(dialog.resultText().isEmpty());
    CHECK(fx.saves == 0);
}

TEST_CASE("a wrong .gguf is refused", "[ui_setup][widget]") {
    Fixture fx;
    fx.nextModel = fx.modelFile("these bytes are not a model");

    fv::RuntimeSetupDialog dialog(shared(fx.ports()));
    fx.settle();
    dialog.modelButton()->click();
    fx.settle();

    CHECK(fx.modelPicks == 1);
    CHECK(dialog.resultText() == text(kUnknownModel));
    CHECK_FALSE(std::filesystem::exists(fx.speechModels / "parakeet-v3"));
    CHECK(dialog.result() != QDialog::Accepted);
    CHECK(dialog.modelButton()->isEnabled());
}

TEST_CASE("a picked model file is imported and, with a runtime in place, finishes the setup",
          "[ui_setup][widget]") {
    Fixture fx;
    fx.openwhisperRuntime();
    fx.adopter().adopt_component("asr-nvidia-cpu");
    const auto file = fx.modelFile();
    fx.nextModel = file;

    fv::RuntimeSetupDialog dialog(shared(fx.ports()));
    fx.settle();
    dialog.modelButton()->click();
    fx.settle();

    CHECK(dialog.resultText() == QStringLiteral("Imported Parakeet TDT 0.6B v3."));
    CHECK(dialog.usable());
    CHECK(dialog.result() == QDialog::Accepted);
    CHECK(std::filesystem::exists(fx.speechModels / "parakeet-v3" / "installed.json"));
    // The picked file stays where it was.
    CHECK(readFile(file) == kWeights);
}

TEST_CASE("without an OpenWhisper install the dialog asks once for the runtime and the model",
          "[ui_setup][widget]") {
    Fixture fx;
    REQUIRE_FALSE(std::filesystem::exists(fx.ow));
    fx.nextFolder = fx.runtimeFolder();
    fx.nextModel = fx.modelFile();

    {
        fv::RuntimeSetupDialog dialog(shared(fx.ports()));
        fx.settle();
        CHECK(dialog.offered().isEmpty());
        dialog.folderButton()->click();
        fx.settle();
        CHECK(dialog.result() != QDialog::Accepted);
        CHECK(
            dialog.readinessText() ==
            QStringLiteral("Speech runtime: Ready\nSpeech model (Parakeet TDT 0.6B v3): Missing"));
        dialog.modelButton()->click();
        fx.settle();
        CHECK(dialog.result() == QDialog::Accepted);
    }

    // The next start: the saved folder and the imported model are found, so the dialog closes on
    // its first look without a single pick.
    fx.folderPicks = 0;
    fx.modelPicks = 0;
    fv::RuntimeSetupDialog next(shared(fx.ports()));
    fx.settle();
    CHECK(next.usable());
    CHECK(next.result() == QDialog::Accepted);
    CHECK(fx.folderPicks == 0);
    CHECK(fx.modelPicks == 0);
}

TEST_CASE("Later closes the dialog and changes nothing", "[ui_setup][widget]") {
    Fixture fx;
    fx.openwhisperRuntime();
    fx.openwhisperModel();
    const auto before = snapshotOf(fx.ow);

    fv::RuntimeSetupDialog dialog(shared(fx.ports()));
    fx.settle();
    dialog.laterButton()->click();
    CHECK(dialog.result() == QDialog::Rejected);
    fx.settle();

    CHECK_FALSE(dialog.usable());
    CHECK(fx.saves == 0);
    CHECK_FALSE(std::filesystem::exists(fx.components));
    CHECK_FALSE(std::filesystem::exists(fx.speechModels));
    CHECK(snapshotOf(fx.ow) == before);
}

TEST_CASE("closing the dialog while an adoption runs cancels it", "[ui_setup][widget]") {
    Fixture fx;
    fx.openwhisperRuntime();
    fx.openwhisperModel();

    fv::RuntimeSetupDialog dialog(shared(fx.ports()));
    fx.settle();
    dialog.useButton()->click();
    REQUIRE(dialog.busy());
    dialog.reject();
    fx.settle();

    CHECK_FALSE(dialog.busy());
    CHECK(dialog.result() == QDialog::Rejected);
    CHECK_FALSE(std::filesystem::exists(fx.components / "asr-nvidia-cpu" / ".installed"));
    CHECK_FALSE(std::filesystem::exists(fx.speechModels / "parakeet-v3" / "installed.json"));
}

TEST_CASE("Later before the first look stays Later when the look finds the engine usable",
          "[ui_setup][widget]") {
    Fixture fx;
    fx.openwhisperRuntime();
    fx.openwhisperModel();
    auto adopter = fx.adopter();
    adopter.adopt_component("asr-nvidia-cpu");
    adopter.adopt_model("parakeet-v3");

    fv::RuntimeSetupDialog dialog(shared(fx.ports()));
    REQUIRE_FALSE(dialog.looked());
    dialog.laterButton()->click();
    fx.settle();

    // The look that was already queued must not accept a dialog the user closed.
    CHECK(dialog.result() == QDialog::Rejected);
    CHECK_FALSE(dialog.usable());
    CHECK_FALSE(dialog.looked());
}

TEST_CASE("a look that finishes after the dialog is gone is dropped", "[ui_setup][widget]") {
    Fixture fx;
    fx.openwhisperRuntime();
    auto dialog = std::make_unique<fv::RuntimeSetupDialog>(shared(fx.ports()));
    REQUIRE(fx.io.queued() == 1);
    dialog.reset();
    // The look runs on io after its dialog was destroyed; its result has nowhere to go.
    fx.settle();
    CHECK(fx.io.queued() == 0);
}

TEST_CASE("an adoption that finishes after the dialog is gone is cancelled and dropped",
          "[ui_setup][widget]") {
    Fixture fx;
    fx.openwhisperRuntime();
    fx.openwhisperModel();
    auto dialog = std::make_unique<fv::RuntimeSetupDialog>(shared(fx.ports()));
    fx.settle();
    dialog->useButton()->click();
    REQUIRE(fx.io.queued() == 1);
    dialog.reset();
    fx.settle();
    CHECK(fx.io.queued() == 0);
    CHECK_FALSE(std::filesystem::exists(fx.components / "asr-nvidia-cpu" / ".installed"));
    CHECK_FALSE(std::filesystem::exists(fx.speechModels / "parakeet-v3" / "installed.json"));
}

TEST_CASE("results that race the dialog's destruction on a real thread are dropped",
          "[ui_setup][widget]") {
    Fixture fx;
    fx.openwhisperRuntime();
    fx.openwhisperModel();
    fv::ThreadPool pool("fv-test-io", 2);
    auto ports = fx.ports();
    ports.io = &pool;
    const auto shared_ports = shared(std::move(ports));
    // Each dialog posts its look at once and is destroyed while the pool may be delivering it.
    for (int round = 0; round < 50; ++round) {
        auto dialog = std::make_unique<fv::RuntimeSetupDialog>(shared_ports);
        if (round % 2 == 0) {
            QCoreApplication::processEvents();
        }
        dialog.reset();
    }
    pool.shutdown(std::chrono::seconds(30));
    QCoreApplication::processEvents();
    SUCCEED("no result reached a destroyed dialog");
}

TEST_CASE("a failing look still opens the dialog", "[ui_setup][widget]") {
    Fixture fx;
    auto ports = fx.ports();
    ports.discover = []() -> fv::models::Discovery {
        throw std::runtime_error("the install is unreadable");
    };
    ports.resolve = []() -> fv::models::ResolvedEngine {
        throw std::runtime_error("the settings are unreadable");
    };
    fv::RuntimeSetupDialog dialog(shared(std::move(ports)));
    fx.settle();
    CHECK(dialog.looked());
    CHECK(dialog.offered().isEmpty());
    CHECK_FALSE(dialog.usable());
    CHECK(dialog.folderButton()->isEnabled());
}

TEST_CASE("ensureEngine returns false when the user chooses Later", "[ui_setup][widget]") {
    Fixture fx;
    fx.openwhisperRuntime();
    fv::RuntimeSetup setup(fx.ports());

    QObject guard;
    QTimer::singleShot(0, &guard, [] {
        if (auto* dialog =
                qobject_cast<fv::RuntimeSetupDialog*>(QApplication::activeModalWidget())) {
            dialog->laterButton()->click();
        }
    });
    CHECK_FALSE(setup.ensureEngine(nullptr));
}

TEST_CASE("ensureEngine returns true, and closes at once, when the engine is already set up",
          "[ui_setup][widget]") {
    Fixture fx;
    fx.openwhisperRuntime();
    fx.openwhisperModel();
    auto adopter = fx.adopter();
    adopter.adopt_component("asr-nvidia-cpu");
    adopter.adopt_model("parakeet-v3");

    // A real thread, because the dialog's event loop runs while it waits for its first look.
    fv::ThreadPool pool("fv-test-io", 2);
    auto ports = fx.ports();
    ports.io = &pool;
    fv::RuntimeSetup setup(std::move(ports));

    QObject guard;
    QTimer::singleShot(10000, &guard, [] {
        if (auto* dialog =
                qobject_cast<fv::RuntimeSetupDialog*>(QApplication::activeModalWidget())) {
            dialog->laterButton()->click();
        }
    });
    CHECK(setup.ensureEngine(nullptr));
}

TEST_CASE("setRuntimeLoaded reaches the dialogs ensureEngine opens", "[ui_setup][widget]") {
    Fixture fx;
    fx.nextFolder = fx.runtimeFolder();
    REQUIRE_FALSE(fx.runtimeLoaded);
    fv::RuntimeSetup setup(fx.ports());
    setup.setRuntimeLoaded([] { return true; });

    QString shown;
    QObject guard;
    QTimer::singleShot(0, &guard, [&] {
        auto* dialog = qobject_cast<fv::RuntimeSetupDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        fx.settle();
        dialog->folderButton()->click();
        fx.settle();
        shown = dialog->resultText();
        dialog->laterButton()->click();
    });
    // The model is still missing, so the setup is not finished.
    CHECK_FALSE(setup.ensureEngine(nullptr));
    CHECK(fx.saves == 1);
    CHECK_THAT(shown.toStdString(), ContainsSubstring("Restart OpenWhisper_CPP to switch to it."));
    CHECK_THAT(shown.toStdString(),
               ContainsSubstring("Download Parakeet TDT 0.6B v3 in Downloads."));
}

TEST_CASE("SetupFeature provides RuntimeSetup and registers nothing", "[ui_setup]") {
    fv::AppContext context(fv::AppServices{});
    std::vector<std::unique_ptr<fv::Feature>> features;
    features.push_back(std::make_unique<fv::SetupFeature>());
    CHECK(features[0]->id() == "setup");
    fv::FeatureRegistry::run(features, context);
    CHECK(context.find<fv::RuntimeSetup>() != nullptr);
}

TEST_CASE("the app's ports adopt into OpenWhisper_CPP's folders and save the runtime folder",
          "[ui_setup][widget]") {
    const auto tag = fv::models::platform_tag();
    if (!tag) {
        SKIP("This platform has no runtime components");
    }
    TempDir temp;
    const auto ow = temp.path() / "OpenWhisper";
    const auto local = temp.path() / "local";
    // The data-folder variables are named from the identities, as Paths reads them.
    const auto dataDirVariable = [](const fv::AppIdentity& id) {
        return std::string(id.env_prefix) + "DATA_DIR";
    };
    fv::MapEnvReader env{
        {"LOCALAPPDATA", fv::base_utf8::path_to_utf8(local)},
        {"XDG_DATA_HOME", fv::base_utf8::path_to_utf8(local)},
        {"HOME", fv::base_utf8::path_to_utf8(temp.path() / "home")},
        {dataDirVariable(fv::identity()), fv::base_utf8::path_to_utf8(temp.path() / "data")},
        {dataDirVariable(fv::legacy_identity()), fv::base_utf8::path_to_utf8(ow)}};
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), env);
    fv::SettingsStore settings(paths.user_file(fv::identity().settings_file));
    fv::ManualExecutor io;
    fv::AppServices services;
    services.paths = &paths;
    services.settings = &settings;
    services.io = &io;
    fv::AppContext context(services);

    fv::models::testing::write_openwhisper_runtime(ow, "asr-nvidia-cpu", *tag);
    const auto before = snapshotOf(ow);
    const auto folder = temp.path() / "mine";
    writeFile(folder / "bin" / fv::runtime_library_name(), "library");

    auto ports = fv::RuntimeSetup::portsFor(context);
    // The pickers are the one part that would open a window.
    ports.pick_runtime_folder = [&](QWidget*) {
        return std::optional<std::filesystem::path>(folder);
    };
    fv::RuntimeSetupDialog dialog(shared(std::move(ports)));
    const auto settle = [&] {
        for (int round = 0; round < 6; ++round) {
            io.run_all();
            QCoreApplication::processEvents();
        }
    };
    settle();
    CHECK(dialog.offered() == QStringList{QStringLiteral("NVIDIA Speech CPU runtime")});

    dialog.useButton()->click();
    settle();
    CHECK(std::filesystem::exists(paths.components_root() / "asr-nvidia-cpu" / ".installed"));
    CHECK(dialog.readinessText().startsWith(QStringLiteral("Speech runtime: Ready")));
    // No model was adopted, so the setup goes on.
    CHECK(dialog.result() != QDialog::Accepted);
    CHECK_THAT(dialog.resultText().toStdString(),
               ContainsSubstring("Download Parakeet TDT 0.6B v3 in Downloads."));
    CHECK(snapshotOf(ow) == before);

    dialog.folderButton()->click();
    settle();
    CHECK(settings.get(fv::keys::fv_runtime_dir) == fv::base_utf8::path_to_utf8(folder));
}
