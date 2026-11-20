#pragma once

#include <optional>
#include <string>

#include "base/json.h"
#include "base/paths.h"
#include "models/catalog.h"

// What guided setup says about dictation's speech model (services/guided_setup.py
// speech_model_name and speech_model_downloaded), as plain functions over a settings snapshot.
// Parakeet is the only local engine; the API server and a remote host run no local model.
namespace fv::setup_speech {

// The local model dictation runs ("parakeet-v3"), or nullopt for an API server or a remote
// host.
std::optional<std::string> speech_model_name(const Json& settings);

// Whether dictation's local model and its runtime are on this computer: false while the
// runtime the model's device needs is missing (models::missing_runtime) or the model folder is
// incomplete; nullopt when dictation runs no local model, or the model could not be checked.
// Reads the disk only (components under paths.components_root(), models under
// paths.speech_models_root()); never downloads or probes a server. A saved fv_runtime_dir stands
// in for every runtime, as it does for the engine.
std::optional<bool> speech_model_downloaded(const Json& settings, const Paths& paths);
// The same on an explicit host (platform and GPU); its is_installed is replaced by the check
// above.
std::optional<bool> speech_model_downloaded(const Json& settings, const Paths& paths,
                                            models::Host host);

} // namespace fv::setup_speech
