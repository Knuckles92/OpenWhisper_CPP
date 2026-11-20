#include "ui_remote/host_model_switch.h"

#include <exception>
#include <format>

#include "base/log.h"
#include "models/engine_control.h"
#include "models/speech_catalog.h"
#include "settings/keys.h"
#include "settings/store.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"services.runtime.remote"};

// The name in the refusals: the request was made on this computer.
constexpr std::string_view kName = "This computer";

} // namespace

std::optional<std::string> select_host_model(models::EngineControl& engine, SettingsStore& settings,
                                             std::string_view family, std::string_view model) {
    try {
        // _engine_switch_refusal
        if (engine.meeting_active()) {
            return std::format("{} is running a meeting. Change its model after the meeting ends.",
                               kName);
        }
        if (engine.job_active()) {
            return std::format("{} is transcribing right now. Try again in a moment.", kName);
        }
        // _save_engine_model: only a model of the family it names.
        const auto* listed = models::model(model);
        if (listed == nullptr || listed->backend != family) {
            return std::format("{} can't run {}.", kName, model);
        }
        const bool serving =
            engine.selected_backend() == family && engine.loaded_model_key() == listed->key;
        auto saved = keys::local_asr_models.read(*settings.snapshot());
        saved[std::string(family)] = std::string(listed->key);
        settings.update(Json{{std::string(keys::selected_model.key()), std::string(family)},
                             {std::string(keys::local_asr_models.key()), saved}});
        kLog.info("Host Mode switched this computer to {}", listed->label);
        if (serving) {
            return std::nullopt; // Selecting it was all it took.
        }
        engine.reload_engine();
        return std::nullopt;
    } catch (const std::exception& error) {
        kLog.error("Switching the served model failed: {}", error.what());
        return std::format("Couldn't switch models: {}", error.what());
    }
}

} // namespace fv
