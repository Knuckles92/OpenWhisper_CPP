#include "ui_controller/engine_setup.h"

#include <exception>
#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "base/log.h"
#include "base/utf8.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "speech/dictation_language.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"services.application_controller"};

std::string lowered(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

// One line naming a crash: its time, code and module, as the crash log's line has them.
std::string crashKey(const log::CrashInfo& crash) {
    return std::format("{} 0x{:08X} {}\n", crash.utc, crash.code, crash.module);
}

} // namespace

bool crashAlreadyOffered(const std::filesystem::path& file, const log::CrashInfo& crash) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text == crashKey(crash);
}

void rememberCrashOffered(const std::filesystem::path& file, const log::CrashInfo& crash) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << crashKey(crash);
    out.close();
    if (!out) {
        throw std::filesystem::filesystem_error("Could not remember the crash offered", file,
                                                std::make_error_code(std::errc::io_error));
    }
}

bool crashedInRuntime(const std::string& module, const std::filesystem::path& runtimeDir) {
    if (module.empty() || module == "unknown" || runtimeDir.empty()) {
        return false;
    }
    const std::string wanted = lowered(module);
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(
             runtimeDir, std::filesystem::directory_options::skip_permission_denied, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_regular_file(error) && lowered(path_to_utf8(it->path().filename())) == wanted) {
            return true;
        }
    }
    return false;
}

void chooseCpuRuntime(SettingsStore& settings) {
    settings.save_setting(keys::fv_speech_runtime.key(), Json("cpu"));
}

EngineSetup::EngineSetup(EngineSetupPorts ports) : ports_(std::move(ports)) {
    if (ports_.settings == nullptr || ports_.io == nullptr || ports_.ui == nullptr ||
        !ports_.recover_components || !ports_.adopt_missing || !ports_.resolve ||
        !ports_.ensure_engine || !ports_.offer_cpu_runtime) {
        throw std::invalid_argument("EngineSetup needs every port");
    }
}

EngineSetup::~EngineSetup() {
    alive_.reset();
}

void EngineSetup::start(std::function<void()> done) {
    if (started_) {
        return;
    }
    started_ = true;
    const Json settings = *ports_.settings->snapshot();
    std::weak_ptr<int> alive = alive_;
    ports_.io->post([this, alive, settings, done = std::move(done)]() mutable {
        if (!alive.lock()) {
            return;
        }
        // Before any load: a change staged for a loaded runtime is applied now, and a swap an
        // earlier run left half done is put right.
        try {
            ports_.recover_components();
        } catch (const std::exception& error) {
            kLog.error_with(error, "Component startup recovery failed");
        }
        try {
            const models::AdoptionReport report = ports_.adopt_missing(settings, ports_.cancel);
            for (const auto& id : report.components) {
                kLog.info("Adopted the speech runtime {} from an existing install", id);
            }
            for (const auto& key : report.models) {
                kLog.info("Adopted the speech model {} from an existing install", key);
            }
        } catch (const std::exception& error) {
            kLog.error_with(error, "Could not adopt the speech runtime and model");
        }
        std::optional<log::CrashInfo> crashed;
        try {
            if (ports_.last_crash) {
                if (auto crash = ports_.last_crash()) {
                    const models::ResolvedEngine engine = ports_.resolve(settings);
                    if (engine.runtime_dir &&
                        crashedInRuntime(crash->module, *engine.runtime_dir) &&
                        keys::fv_speech_runtime.read(settings) != "cpu" &&
                        (ports_.crash_offered_file.empty() ||
                         !crashAlreadyOffered(ports_.crash_offered_file, *crash))) {
                        crashed = std::move(crash);
                    }
                }
            }
        } catch (const std::exception& error) {
            kLog.warning("Could not read the crash log: {}", error.what());
        }
        ports_.ui->post(
            [this, alive, crashed = std::move(crashed), done = std::move(done)]() mutable {
                if (alive.lock()) {
                    finishOnUi(std::move(crashed), std::move(done));
                }
            });
    });
}

void EngineSetup::finishOnUi(std::optional<log::CrashInfo> crash, std::function<void()> done) {
    if (crash) {
        kLog.warning("The speech engine crashed in the last session");
        if (ports_.offer_cpu_runtime()) {
            chooseCpuRuntime(*ports_.settings);
        }
        // Answered either way: the same crash is not offered again, a later one is.
        if (!ports_.crash_offered_file.empty()) {
            ports_.io->post([file = ports_.crash_offered_file, crash = *crash] {
                try {
                    rememberCrashOffered(file, crash);
                } catch (const std::exception& error) {
                    kLog.warning("Could not remember the crash offered: {}", error.what());
                }
            });
        }
    }
    // The cloud and remote engines need no runtime here.
    if (speech::engine(*ports_.settings->snapshot()) == "parakeet") {
        ensureEngine();
    }
    if (done) {
        done();
    }
}

bool EngineSetup::ensureEngine() {
    if (dialogOpen_) {
        return false;
    }
    const models::ResolvedEngine engine = ports_.resolve(*ports_.settings->snapshot());
    if (engine.missing_state == models::EngineMissing::None) {
        return engine.usable();
    }
    dialogOpen_ = true;
    bool usable = false;
    try {
        usable = ports_.ensure_engine();
    } catch (const std::exception& error) {
        kLog.error_with(error, "The speech engine setup failed");
    }
    dialogOpen_ = false;
    return usable;
}

} // namespace fv
