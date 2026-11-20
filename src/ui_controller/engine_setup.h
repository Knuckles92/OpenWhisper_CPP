#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "base/cancel.h"
#include "base/executor.h"
#include "base/json.h"
#include "log/crash_handler.h"
#include "models/engine_resolution.h"
#include "models/openwhisper_discovery.h"

namespace fv {

class SettingsStore;

// What the startup of the speech engine does to the outside world; every function is required
// except where an empty one is named.
struct EngineSetupPorts {
    SettingsStore* settings = nullptr;
    // The blocking steps (component recovery, adoption, the crash check) run here.
    Executor* io = nullptr;
    Executor* ui = nullptr;
    // ComponentStore::prune_orphans, then settle_staged_changes: a14's startup recovery, before
    // any runtime loads. Throws when it fails.
    std::function<void()> recover_components;
    // OpenWhisperAdopter::adopt_missing over the saved settings, under `cancel`.
    std::function<models::AdoptionReport(const Json& settings, const CancelToken& cancel)>
        adopt_missing;
    // Cancelled at Quit (ShutdownStep::CancelRoot): an adoption copying and hashing gigabytes
    // stops, leaving nothing half adopted, so the io strand Quit waits for is free at once.
    CancelToken cancel;
    // resolve_parakeet over the saved settings.
    std::function<models::ResolvedEngine(const Json& settings)> resolve;
    // RuntimeSetup::ensureEngine over the main window; true when the engine is usable after.
    std::function<bool()> ensure_engine;
    // CrashHandler::last_crash; empty: no crash log.
    std::function<std::optional<log::CrashInfo>()> last_crash;
    // "The speech engine crashed": true when the user takes the CPU runtime. GUI thread.
    std::function<bool()> offer_cpu_runtime;
    // Where the crash last offered is remembered: the crash log is never cleared, so without it
    // a declined offer would come back at every start. Empty: nothing is remembered.
    std::filesystem::path crash_offered_file;
};

// Whether `module` (a file name from the crash log) is one of the files of the runtime folder:
// the NeMo-Speech.cpp library or one of its ggml libraries. Case-insensitive, as Windows names
// are. Blocking (it lists the folder).
bool crashedInRuntime(const std::string& module, const std::filesystem::path& runtimeDir);

// Whether `crash` is the one `file` remembers as offered already. Blocking; false when the file
// is missing or unreadable.
bool crashAlreadyOffered(const std::filesystem::path& file, const log::CrashInfo& crash);
// Remembers `crash` as offered (replacing the one before). Blocking; throws
// std::filesystem::filesystem_error when the file cannot be written.
void rememberCrashOffered(const std::filesystem::path& file, const log::CrashInfo& crash);

// The speech engine's startup (architecture 5.7, 7.6, section 13 item 45), after the main window
// is shown: off the GUI thread the component store recovers interrupted changes and applies
// those staged for this start, then what an OpenWhisper install offers is adopted, and the crash
// log is read; back on the GUI thread a crash inside the runtime offers the CPU runtime (once per
// crash: the offer is remembered whatever the answer), and the
// setup dialog opens only when the Parakeet engine still has no runtime or model. Then `done`
// runs, which loads the engine.
// Affinity: GUI thread (start, ensureModel); the steps it posts run on io.
class EngineSetup {
public:
    explicit EngineSetup(EngineSetupPorts ports);
    ~EngineSetup();
    EngineSetup(const EngineSetup&) = delete;
    EngineSetup& operator=(const EngineSetup&) = delete;

    // Runs once; `done` runs on the GUI thread even when a step failed (each failure is logged).
    void start(std::function<void()> done);
    // The selected model or runtime is missing at a later readiness check: the setup dialog,
    // when resolve_parakeet still reports one missing. True when the engine is usable.
    bool ensureEngine();
    bool started() const { return started_; }

private:
    void finishOnUi(std::optional<log::CrashInfo> crash, std::function<void()> done);

    EngineSetupPorts ports_;
    bool started_ = false;
    bool dialogOpen_ = false;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

// fv_speech_runtime is "cpu" from the next start (section 5.7: the runtime is fixed per process).
void chooseCpuRuntime(SettingsStore& settings);

} // namespace fv
