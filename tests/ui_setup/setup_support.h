#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>

#include <QApplication>
#include <QCoreApplication>
#include <QString>

#include "base/executor.h"
#include "base/json.h"
#include "base/testing/manual_executor.h"
#include "base/utf8.h"
#include "engine/runtime.h"
#include "models/catalog.h"
#include "models/engine_resolution.h"
#include "models/openwhisper_discovery.h"
#include "models/testing/fake_openwhisper.h"
#include "ui_setup/runtime_setup.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"

namespace fvtest {

namespace fs = std::filesystem;

// A fresh folder under the system temp folder, removed with everything in it at the end.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        std::random_device seed;
        path_ = fs::temp_directory_path() /
                ("fv-ui-setup-test-" + std::to_string(seed()) + "-" + std::to_string(++counter));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

inline void writeFile(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

inline std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Every file under `root` with its bytes and modification time: a tree that setup must leave
// exactly as it was.
inline std::map<std::string, std::pair<std::string, fs::file_time_type>>
snapshotOf(const fs::path& root) {
    std::map<std::string, std::pair<std::string, fs::file_time_type>> files;
    if (!fs::exists(root)) {
        return files;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            files[fv::base_utf8::path_to_utf8(entry.path().lexically_relative(root))] = {
                readFile(entry.path()), entry.last_write_time()};
        }
    }
    return files;
}

// The application styled the way OpenWhisper's QtApplication styles it, for one test case.
class AppTheme {
public:
    explicit AppTheme(const QString& theme, int scalePercent = 100) : manager_(theme) {
        fv::ui::applyUiTheme(theme, *qApp, manager_);
        fv::ui::applyUiFontScale(scalePercent, *qApp, manager_);
    }
    ~AppTheme() { qApp->setStyleSheet(QString()); }
    AppTheme(const AppTheme&) = delete;
    AppTheme& operator=(const AppTheme&) = delete;

private:
    fv::ui::ThemeManager manager_;
};

inline const std::string kWeights = "parakeet weights";

// An OpenWhisper install and a FioriVoce data folder in a temporary folder, and the ports of the
// setup dialog wired to them: the adopter is the real one, over those folders; the engine
// resolution and the settings are stand-ins, because the real model pin is a 714 MB file; the
// pickers answer what the test queued. Work posted to the io executor waits for settle().
struct Fixture {
    TempDir temp;
    fs::path ow = temp.path() / "OpenWhisper";
    fs::path components = temp.path() / "FioriVoce" / "components";
    fs::path speechModels = temp.path() / "FioriVoce" / "speech-models";
    fv::Json spec = fv::models::testing::small_model_spec("parakeet.gguf", kWeights);
    fv::ManualExecutor io;

    fs::path savedRuntimeDir;
    int saves = 0;
    bool runtimeLoaded = false;
    std::optional<fs::path> nextFolder;
    std::optional<fs::path> nextModel;
    int folderPicks = 0;
    int modelPicks = 0;

    fv::models::OpenWhisperAdopter adopter() {
        fv::models::AdopterOptions options;
        options.artifacts = [this](std::string_view) {
            return spec;
        };
        fv::models::Host host;
        host.platform = "win_amd64";
        return fv::models::OpenWhisperAdopter(components, speechModels, {ow}, host, options);
    }

    fs::path openwhisperRuntime() {
        return fv::models::testing::write_openwhisper_runtime(ow, "asr-nvidia-cpu", "win_amd64");
    }
    fs::path openwhisperModel(const std::string& weights = kWeights) {
        return fv::models::testing::write_openwhisper_model(ow, "parakeet-v3", spec,
                                                            {{"parakeet.gguf", weights}});
    }
    // A folder holding the runtime library where the release puts it.
    fs::path runtimeFolder(std::string_view name = "my-runtime") {
        const fs::path folder = temp.path() / std::string(name);
        writeFile(folder / "bin" / fv::runtime_library_name(), "library");
        return folder;
    }
    fs::path modelFile(std::string_view bytes = kWeights, std::string_view name = "my.gguf") {
        const fs::path file = temp.path() / std::string(name);
        writeFile(file, bytes);
        return file;
    }

    fv::models::ResolvedEngine resolve() const {
        fv::models::ResolvedEngine resolved;
        resolved.model_key = "parakeet-v3";
        resolved.model_label = "Parakeet TDT 0.6B v3";
        resolved.device = "cpu";
        resolved.requested_device = "auto";
        const bool runtime =
            !savedRuntimeDir.empty() || fs::exists(components / "asr-nvidia-cpu" / ".installed");
        if (!runtime) {
            resolved.missing_state = fv::models::EngineMissing::RuntimeMissing;
            resolved.missing_message = "Install Parakeet's CPU runtime in Downloads.";
        } else {
            resolved.runtime_dir =
                savedRuntimeDir.empty() ? components / "asr-nvidia-cpu" : savedRuntimeDir;
        }
        if (!fs::exists(speechModels / "parakeet-v3" / "installed.json")) {
            if (runtime) {
                resolved.missing_state = fv::models::EngineMissing::ModelMissing;
                resolved.missing_message = "Download Parakeet TDT 0.6B v3 in Downloads.";
            }
            return resolved;
        }
        resolved.model_path = speechModels / "parakeet-v3" / "parakeet.gguf";
        return resolved;
    }

    fv::RuntimeSetupPorts ports() {
        fv::RuntimeSetupPorts ports;
        ports.io = &io;
        ports.discover = [this] {
            return adopter().discover();
        };
        ports.adopt_component = [this](const std::string& id, const fv::CancelToken& cancel) {
            adopter().adopt_component(id, cancel);
        };
        ports.adopt_model = [this](const std::string& key, const fv::CancelToken& cancel) {
            adopter().adopt_model(key, cancel);
        };
        ports.import_model_file = [this](const fs::path& file, const fv::CancelToken& cancel) {
            return adopter().import_model_file(file, cancel);
        };
        ports.resolve = [this] {
            return resolve();
        };
        ports.save_runtime_dir = [this](const fs::path& folder) {
            savedRuntimeDir = folder;
            ++saves;
        };
        ports.runtime_loaded = [this] {
            return runtimeLoaded;
        };
        ports.pick_runtime_folder = [this](QWidget*) {
            ++folderPicks;
            return nextFolder;
        };
        ports.pick_model_file = [this](QWidget*) {
            ++modelPicks;
            return nextModel;
        };
        return ports;
    }

    // Runs what the dialog posted to the io executor and delivers the results to the GUI thread,
    // until neither has more.
    void settle() {
        for (int round = 0; round < 6; ++round) {
            io.run_all();
            QCoreApplication::processEvents();
        }
    }
};

} // namespace fvtest
