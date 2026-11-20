#include "meeting/setup_check.h"

#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "base/log.h"
#include "meeting_capture/devices.h"
#include "meeting_capture/health.h"
#include "meeting_capture/meeting_clock.h"
#include "meeting_capture/sources.h"
#include "meeting_state/types.h"
#include "models/catalog.h"
#include "models/hf_access_policy.h"
#include "models/meeting_speech.h"
#include "models/runtime_resolver.h"
#include "models/speaker_model.h"
#include "models/speech_catalog.h"
#include "settings/keys.h"

namespace fv::meeting {

namespace {

constexpr log::Logger logger("services.meeting_setup");

std::mutex& audio_check_lock() {
    static std::mutex lock;
    return lock;
}

const Clock& clock_of(const SetupDeps& deps) {
    return deps.clock != nullptr ? *deps.clock : system_clock();
}

// One open device of the check and the monitor its blocks feed. The monitor is declared first so
// it outlives the source: a source whose stop() threw may still deliver a block while it goes.
struct OpenSource {
    std::unique_ptr<CaptureSignalMonitor> monitor;
    std::unique_ptr<CaptureSource> source;
};

std::string result_of(const CaptureSignalMonitor* monitor) {
    if (monitor == nullptr) {
        return "Could not open";
    }
    const CaptureHealth health = monitor->snapshot();
    if (health.signal_windows != 0) {
        return std::string(kSignalDetected);
    }
    return health.receiving ? "No signal detected" : "No audio received";
}

} // namespace

SetupDeps setup_deps(const SetupServices& services) {
    SetupDeps deps;
    SettingsStore* settings = &services.settings;
    const EnvReader* env = &services.env;
    const Paths* paths = &services.paths;
    http::Client* client = &services.client;
    models::SpeechModelCache* cache = &services.speech_models;
    models::ComponentCoordinator* components = &services.components;
    audio::Backend* backend = &services.audio;

    deps.settings = [settings] {
        return *settings->snapshot();
    };
    deps.resolve_runtime = [components](std::string_view model_backend, std::string_view device) {
        return models::resolve_runtime(model_backend, device, components->store().host()).first;
    };
    deps.is_installed = [components](std::string_view component) {
        return components->store().is_installed(component);
    };
    deps.is_model_cached = [cache](std::string_view model) {
        return cache->is_cached(model);
    };
    deps.speaker_model_path = [paths, env] {
        return models::speaker_model_path(*paths, *env);
    };
    deps.components = components;
    deps.hf_access = &services.hf_access;
    deps.catalog_entry = [components](std::string_view component) {
        return components->catalog_entry(component);
    };
    deps.check_compatibility = [components](const Json& entry) {
        return models::check_compatibility(entry, components->store().host()).value_or("");
    };
    deps.hf_offline_env = [env] {
        return models::is_hf_hub_offline_env_set(*env);
    };
    deps.hf_access_policy = [settings] {
        return models::load_hf_access_policy(*settings);
    };
    deps.download_model = [cache](std::string_view model, const CancelToken& cancel) {
        cache->download(model, {}, cancel);
    };
    deps.describe_download_error = [](std::string_view model, const std::exception_ptr& error) {
        return models::describe_model_download_error(model, error);
    };
    deps.install_component = [components](std::string_view component, const Json& entry,
                                          const CancelToken& cancel) {
        components->store().install(component, entry, {}, cancel);
    };
    deps.speaker_model_download_blocked = [settings, env] {
        return models::speaker_model_download_blocked(*settings->snapshot(), *env);
    };
    deps.ensure_speaker_model = [paths, env, settings, client](const CancelToken& cancel) {
        models::SpeakerModelDeps model_deps(*paths, *env, *settings, *client);
        models::ensure_speaker_model(model_deps, cancel);
    };
    deps.mic_priority = [settings, backend] {
        return audio::load_priority(*settings, *backend);
    };
    return deps;
}

bool setup_complete(const Json& settings) {
    return keys::meeting_setup_complete.read(settings);
}

std::vector<std::string> ModelSetup::missing() const {
    std::vector<std::string> names;
    if (!speech_download.empty()) {
        names.push_back(label);
    }
    if (!runtime.empty()) {
        names.emplace_back("speech runtime");
    }
    if (speaker_download) {
        names.emplace_back("speaker identification model");
    }
    return names;
}

ModelSetup inspect_models(const SetupDeps& deps) {
    const Json settings = deps.settings ? deps.settings() : Json::object();
    // Any backend but "off" runs the on-device diarizer, which needs the model.
    const bool speaker_download =
        models::resolve_meeting_speaker_id_backend(settings, clock_of(deps)) != "off" &&
        !(deps.speaker_model_path && deps.speaker_model_path());
    if (models::resolve_meeting_asr_source(settings) == "remote") {
        return ModelSetup{"Remote speech engine", "", "", speaker_download};
    }
    const std::string model = models::resolve_meeting_whisper_model(settings);
    const models::SpeechModel* entry = models::model(model);
    std::string runtime;
    if (entry != nullptr) {
        runtime =
            deps.resolve_runtime(entry->backend, models::selected_device(settings, entry->backend));
        if (deps.is_installed(runtime)) {
            runtime.clear();
        }
    }
    ModelSetup setup;
    setup.label = entry != nullptr ? std::string(entry->label) : model;
    setup.speech_download = deps.is_model_cached(model) ? std::string() : model;
    setup.runtime = std::move(runtime);
    setup.speaker_download = speaker_download;
    return setup;
}

ModelSetup prepare_models(const SetupDeps& deps, const CancelToken& cancel,
                          const std::function<void(std::string)>& status) {
    const ModelSetup plan = inspect_models(deps);
    if (cancel.cancelled()) {
        return plan;
    }
    const auto report = [&status](std::string text) {
        if (status) {
            status(std::move(text));
        }
    };
    std::optional<Json> entry;
    if (!plan.runtime.empty()) {
        entry = deps.catalog_entry ? deps.catalog_entry(plan.runtime) : std::nullopt;
        if (!entry) {
            throw std::runtime_error(
                "The selected speech runtime is unavailable on this computer.");
        }
        const std::string reason = deps.check_compatibility(*entry);
        if (!reason.empty()) {
            throw std::runtime_error(reason);
        }
    }
    if (!plan.speech_download.empty()) {
        const std::string& model = plan.speech_download;
        if (deps.hf_offline_env() || deps.hf_access_policy() == models::hf_access_policy::kNever) {
            throw std::runtime_error("Speech model downloads are disabled in Downloads settings.");
        }
        if (!deps.hf_access->begin_request(model)) {
            throw std::runtime_error(
                "The speech model is already downloading. Try again when it finishes.");
        }
        struct EndRequest {
            models::HuggingFaceAccessCoordinator* access;
            const std::string& model;
            ~EndRequest() { access->end_request(model); }
        } end_request{deps.hf_access, model};
        if (!deps.is_model_cached(model)) {
            report("Downloading " + plan.label + "…");
            try {
                deps.download_model(model, cancel);
            } catch (const std::exception&) {
                throw std::runtime_error(
                    deps.describe_download_error(model, std::current_exception()));
            }
        }
    }
    if (!plan.runtime.empty() && !cancel.cancelled()) {
        if (!deps.components->begin_install(plan.runtime)) {
            throw std::runtime_error(
                "The speech runtime is already installing. Try again when it finishes.");
        }
        struct EndInstall {
            models::ComponentCoordinator* components;
            const std::string& runtime;
            ~EndInstall() { components->end_install(runtime); }
        } end_install{deps.components, plan.runtime};
        if (!deps.is_installed(plan.runtime)) {
            report("Installing the speech runtime…");
            deps.install_component(plan.runtime, *entry, cancel);
        }
    }
    if (plan.speaker_download && !cancel.cancelled()) {
        const std::string reason = deps.speaker_model_download_blocked();
        if (!reason.empty()) {
            throw std::runtime_error(reason);
        }
        report("Downloading the speaker identification model…");
        deps.ensure_speaker_model(cancel);
    }
    return inspect_models(deps);
}

AudioCheck check_audio(audio::Backend& backend, const SetupDeps& deps, const CancelToken& cancel,
                       bool system_audio_allowed, std::chrono::milliseconds duration) {
    std::unique_lock lock(audio_check_lock(), std::try_to_lock);
    if (!lock.owns_lock()) {
        throw std::runtime_error("An audio check is still closing. Try again in a moment.");
    }
    const Clock& clock = clock_of(deps);
    const MeetingClock meeting_clock(clock);
    // The sources share the caller's backend without owning it.
    const std::shared_ptr<audio::Backend> shared(std::shared_ptr<void>(), &backend);
    // Opened in order, stopped in reverse (Python's ExitStack).
    std::vector<OpenSource> opened;

    const auto open_source = [&](std::string_view channel, auto make_source,
                                 double signal_rms) -> CaptureSignalMonitor* {
        if (cancel.cancelled()) {
            return nullptr;
        }
        auto monitor = std::make_unique<CaptureSignalMonitor>(clock, signal_rms);
        try {
            std::unique_ptr<CaptureSource> source = make_source();
            if (!source) {
                return nullptr;
            }
            CaptureSignalMonitor* observer = monitor.get();
            CaptureSource* started = source.get();
            opened.push_back({std::move(monitor), std::move(source)});
            started->start([observer](const CaptureBlock& block) { observer->observe(block); });
            if (started->is_active()) {
                return observer;
            }
        } catch (const std::exception& error) {
            logger.warning("Could not open {} for the meeting audio check: {}", channel,
                           error.what());
        }
        return nullptr;
    };

    CaptureSignalMonitor* mic = open_source(
        meeting_state::kChannelMic,
        [&]() -> std::unique_ptr<CaptureSource> {
            const std::vector<audio::DeviceKey> priority =
                deps.mic_priority ? deps.mic_priority() : std::vector<audio::DeviceKey>();
            const std::optional<audio::InputDevice> device = find_mic_device(backend, priority);
            if (!device) {
                return nullptr;
            }
            return std::make_unique<MicSource>(shared, meeting_clock,
                                               std::string(meeting_state::kChannelMic),
                                               device->index, device->samplerate, device->channels);
        },
        SETUP_MIC_SIGNAL_RMS);
    CaptureSignalMonitor* loopback = nullptr;
    if (!cancel.cancelled() && system_audio_allowed) {
        loopback = open_source(
            meeting_state::kChannelLoopback,
            [&]() -> std::unique_ptr<CaptureSource> {
                const std::optional<audio::LoopbackEndpoint> endpoint =
                    find_loopback_endpoint(backend);
                if (!endpoint) {
                    return nullptr;
                }
                return std::make_unique<LoopbackSource>(shared, meeting_clock, *endpoint);
            },
            SIGNAL_RMS_INT16);
    }

    const auto waiting = [&] {
        for (const CaptureSignalMonitor* monitor : {mic, loopback}) {
            if (monitor != nullptr && monitor->snapshot().signal_windows == 0) {
                return true;
            }
        }
        return false;
    };
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline && waiting()) {
        if (cancel.wait_for(std::chrono::milliseconds(100))) {
            break;
        }
    }

    AudioCheck check{result_of(mic), system_audio_allowed
                                         ? result_of(loopback)
                                         : "Screen & System Audio Recording permission needed"};
    for (auto it = opened.rbegin(); it != opened.rend(); ++it) {
        try {
            it->source->stop();
        } catch (const std::exception& error) {
            logger.warning("Could not stop {} after the meeting audio check: {}",
                           it->source->channel(), error.what());
        }
    }
    return check;
}

} // namespace fv::meeting
