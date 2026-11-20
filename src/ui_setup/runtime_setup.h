#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "base/cancel.h"
#include "base/executor.h"
#include "models/engine_resolution.h"
#include "models/openwhisper_discovery.h"

class QWidget;

namespace fv {

class AppContext;

// Everything the setup dialog does to the outside world. The blocking functions run on `io`
// (hashing a 714 MB model must not hold the GUI thread); the two pickers run on the GUI thread.
// An unset function is a failure of the program, not of the user, so portsFor sets them all.
struct RuntimeSetupPorts {
    Executor* io = nullptr;

    // What an OpenWhisper install offers that OpenWhisper_CPP does not have yet.
    std::function<models::Discovery()> discover;
    // Throw models::AdoptionError or models::ComponentError with a sentence for the user;
    // models::ComponentCanceled when `cancel` fired.
    std::function<void(const std::string& component_id, const CancelToken&)> adopt_component;
    std::function<void(const std::string& key, const CancelToken&)> adopt_model;
    // Returns the key of the model the file is; throws models::AdoptionError otherwise.
    std::function<std::string(const std::filesystem::path&, const CancelToken&)> import_model_file;
    // What the Parakeet engine would load now, from the saved settings.
    std::function<models::ResolvedEngine()> resolve;
    // Saves fv_runtime_dir.
    std::function<void(const std::filesystem::path&)> save_runtime_dir;
    // Whether a speech runtime is already loaded in this process, which a new fv_runtime_dir
    // only takes effect after a restart (architecture 5.7). Empty: none is.
    std::function<bool()> runtime_loaded;

    // nullopt when the user cancels the picker.
    std::function<std::optional<std::filesystem::path>(QWidget* parent)> pick_runtime_folder;
    std::function<std::optional<std::filesystem::path>(QWidget* parent)> pick_model_file;
};

// The runtime and model setup M1.16 runs when the Parakeet engine has neither after automatic
// adoption (models::OpenWhisperAdopter::adopt_missing) found nothing to adopt. Guided setup
// (M2) extends the library; this is the modal part.
// Affinity: GUI thread.
class RuntimeSetup {
public:
    // The real ports (portsFor), taken from the context when the dialog first opens, so a
    // context that lacks a service the dialog needs fails there and not at startup.
    explicit RuntimeSetup(AppContext& context);
    explicit RuntimeSetup(RuntimeSetupPorts ports);

    // Shows the setup dialog modally and returns whether the Parakeet engine has a usable
    // runtime and model once it closes. Call it only when resolve_parakeet reported one
    // missing; a dialog that finds both in place on its first look closes itself.
    bool ensureEngine(QWidget* parent);

    // Whether a speech runtime is loaded in this process (RuntimeSetupPorts::runtime_loaded).
    // Nothing below the controller knows, so M1.16 wires it to the speech service here; it
    // replaces the ports' function for the dialogs ensureEngine opens from then on.
    void setRuntimeLoaded(std::function<bool()> loaded);

    // The app's paths and settings, QFileDialog pickers and the io executor. runtime_loaded is
    // left empty (see setRuntimeLoaded).
    static RuntimeSetupPorts portsFor(AppContext& context);

private:
    AppContext* context_ = nullptr;
    std::optional<RuntimeSetupPorts> ports_;
    std::function<bool()> runtimeLoaded_;
};

} // namespace fv
