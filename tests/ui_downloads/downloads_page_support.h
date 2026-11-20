#pragma once

// What the Downloads page's tests share: the page over fakes (an in-memory settings file, a
// manual io executor, an environment, an inventory the test fills, components it describes),
// as OpenWhisper's _DialogTestCase builds DownloadsPage with patched scanners; and the
// catalog's two models standing in for the Whisper rows Python's tests name ("tiny" is
// Orukeet, which sorts first; "base" is Parakeet TDT 0.6B v3, the default dictation model).

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QString>

#include "base/json.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "models/catalog.h"
#include "models/component_details.h"
#include "models/consent_text.h"
#include "models/hf_access.h"
#include "settings/testing/memory_store.h"
#include "ui_downloads/downloads_page.h"
#include "ui_downloads_support.h"

namespace fvtest {

inline const std::string kParakeet = "parakeet-v3";
inline const std::string kOrukeet = "orukeet-v0.1";
inline const std::string kParakeetRepo = "nvidia/parakeet-tdt-0.6b-v3";
inline const std::string kOrukeetRepo = "oruk/orukeet";
inline constexpr std::int64_t kParakeetBytes = 713'975'456;
inline constexpr std::int64_t kOrukeetBytes = 714'456'704;
inline const std::string kCpuRuntime = "asr-nvidia-cpu";
inline const std::string kCudaRuntime = "asr-nvidia-cuda";
inline const QString kModelsFolder =
    QStringLiteral("C:/Users/test/AppData/Local/OpenWhisper_CPP/speech-models");

inline fv::models::CachedModelInfo cachedModel(const std::string& repo, std::int64_t bytes) {
    fv::models::CachedModelInfo info;
    info.repo_id = repo;
    info.size_on_disk = bytes;
    info.path = "/speech-models/" + repo;
    return info;
}

// A runtime as the coordinator describes it on Windows x64.
inline fv::models::ComponentInfo
runtimeInfo(const std::string& id,
            fv::models::ComponentState state = fv::models::ComponentState::NotInstalled) {
    fv::models::ComponentInfo info;
    info.component_id = id;
    std::tie(info.display_name, info.summary) = fv::models::component_copy(id);
    info.state = state;
    const fv::models::RuntimeSizes sizes = fv::models::runtime_sizes(id, windowsHost());
    info.download_bytes = sizes.download;
    info.install_bytes = sizes.on_disk;
    return info;
}

// The page over fakes; every member a test touches is public. OpenWhisper's tests scan
// synchronously (background_cache_scan=False), and so does the rig unless a test says otherwise.
class DownloadsRig {
public:
    explicit DownloadsRig(fv::Json saved = fv::Json{{"selected_model", "parakeet"}})
        : settings(std::move(saved)) {
        components = {runtimeInfo(kCpuRuntime), runtimeInfo(kCudaRuntime)};
    }

    fv::DownloadsPageOptions options() {
        fv::DownloadsPageOptions made;
        made.peekInventory =
            [this]() -> std::optional<fv::models::CachedModelsSnapshot::Inventory> {
            if (!scanned) {
                return std::nullopt;
            }
            return inventory;
        };
        made.scanInventory = [this] {
            ++scans;
            if (scanFails) {
                throw std::runtime_error("the models folder cannot be read");
            }
            scanned = true;
            return inventory;
        };
        made.loadedModel = [this] {
            return loaded;
        };
        made.meetingModel = [this](const fv::Json&) {
            return meeting;
        };
        made.listComponents = [this] {
            return components;
        };
        made.describeComponent = [this](const std::string& id) {
            if (describeFails) {
                throw std::runtime_error("the components folder cannot be read");
            }
            for (const fv::models::ComponentInfo& info : components) {
                if (info.component_id == id) {
                    return info;
                }
            }
            return runtimeInfo(id);
        };
        made.isInstalling = [this](const std::string& id) {
            return installing.contains(id);
        };
        made.env = &env;
        made.modelsFolder = folder;
        made.backgroundScan = background;
        return made;
    }

    // _make_dialog: the page built and refreshed once, as Settings refreshes the page it hosts.
    std::unique_ptr<fv::DownloadsPage> page(bool deferBuild = false) {
        auto made = std::make_unique<fv::DownloadsPage>(settings, io, options(), deferBuild);
        made->refresh();
        return made;
    }

    void cache(const std::string& repo, std::int64_t bytes) {
        inventory.insert_or_assign(repo, cachedModel(repo, bytes));
    }
    void blockDownloads() { env.set("HF_HUB_OFFLINE", "1"); }

    fv::MemorySettingsStore settings;
    fv::ManualExecutor io;
    fv::MapEnvReader env;
    fv::models::CachedModelsSnapshot::Inventory inventory;
    std::optional<std::string> loaded;
    std::string meeting;
    std::vector<fv::models::ComponentInfo> components;
    std::set<std::string> installing;
    // The speech models folder the page names and opens.
    QString folder = kModelsFolder;
    bool background = false;
    bool scanned = false;
    bool scanFails = false;
    bool describeFails = false;
    int scans = 0;
};

inline void pick(QComboBox* combo, const char* data) {
    combo->setCurrentIndex(combo->findData(QString::fromUtf8(data)));
}

} // namespace fvtest
