#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fv {
class SettingsStore;
namespace models {
class EngineControl;
}

// Host Mode's engine picker: serve another model ready on this computer
// (services/runtime/remote.py's RemoteRuntime.select_host_model with _switch_engine_to for a
// request made here). It refuses while a meeting runs or a recording, transcription or reload
// is under way, saves the model the way the engine fields do (selected_model and
// local_asr_models[family]) and queues the reload; nothing reloads when that model already
// serves. Returns why it can't, or nullopt once the switch is under way.
// Affinity: the UI executor (reload_engine runs there only).
std::optional<std::string> select_host_model(models::EngineControl& engine, SettingsStore& settings,
                                             std::string_view family, std::string_view model);

} // namespace fv
