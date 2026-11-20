#include "ui_setup/setup_speech.h"

#include <exception>
#include <filesystem>
#include <string_view>
#include <system_error>

#include "base/log.h"
#include "base/utf8.h"
#include "models/engine_resolution.h"
#include "models/runtime_resolver.h"
#include "models/speech_catalog.h"
#include "models/speech_model_cache.h"
#include "settings/keys.h"

namespace fv::setup_speech {

namespace {

constexpr log::Logger kLog{"services.guided_setup"};

} // namespace

std::optional<std::string> speech_model_name(const Json& settings) {
    const std::string engine = keys::selected_model.read(settings);
    if (models::backend(engine) != nullptr) {
        return models::selected_model(settings, engine);
    }
    return std::nullopt;
}

std::optional<bool> speech_model_downloaded(const Json& settings, const Paths& paths) {
    return speech_model_downloaded(settings, paths, models::Host::current({}));
}

std::optional<bool> speech_model_downloaded(const Json& settings, const Paths& paths,
                                            models::Host host) {
    const std::optional<std::string> model = speech_model_name(settings);
    if (!model) {
        return std::nullopt;
    }
    // resolve_parakeet's answer: a chosen runtime folder counts only when it holds the runtime
    // library (and a GPU build for a GPU component), else the component's sentinel.
    host.is_installed = models::engine_runtime_installed(settings, paths.components_root());
    if (models::missing_runtime(*model, settings, host)) {
        return false;
    }
    try {
        return models::is_model_folder_complete(paths.speech_models_root() /
                                                    fv::base_utf8::path_from_utf8(*model),
                                                models::artifacts(*model));
    } catch (const std::exception& error) {
        kLog.debug("Could not check the speech model cache: {}", error.what());
        return std::nullopt;
    }
}

} // namespace fv::setup_speech
