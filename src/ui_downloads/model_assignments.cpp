#include "ui_downloads/model_assignments.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <format>
#include <optional>
#include <stdexcept>

#include "base/clock.h"
#include "base/utf8.h"
#include "models/component_details.h"
#include "models/engine_resolution.h"
#include "models/meeting_speech.h"
#include "models/runtime_resolver.h"
#include "models/speech_catalog.h"
#include "remote/meeting_route.h"
#include "settings/enums.h"
#include "settings/keys.h"
#include "speech/endpoints.h"

namespace fv {
namespace {

// protocol.DEFAULT_PORT of the remote engine wire.
constexpr std::int64_t kDefaultRemotePort = 47821;

std::chrono::year_month_day local_today() {
    const Clock& clock = system_clock();
    const auto local = clock.zone()->to_local(clock.utc());
    return std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(local)};
}

// int(raw.get("port", DEFAULT_PORT)): an integer, a float cut toward zero, a bool, or text of
// digits; anything else is a ValueError or TypeError, which makes the pairing unusable.
std::optional<std::int64_t> pairing_port(const Json& raw) {
    const auto found = raw.find("port");
    if (found == raw.end()) {
        return kDefaultRemotePort;
    }
    if (found->is_boolean()) {
        return found->get<bool>() ? 1 : 0;
    }
    if (found->is_number_integer()) {
        return found->get<std::int64_t>();
    }
    if (found->is_number_float()) {
        const double value = found->get<double>();
        if (!std::isfinite(value) || std::fabs(value) >= 9.0e18) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(std::trunc(value));
    }
    if (found->is_string()) {
        const std::string& text = found->get_ref<const std::string&>();
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return std::nullopt;
        }
        const auto last = text.find_last_not_of(" \t\r\n");
        std::string digits = text.substr(first, last - first + 1);
        bool negative = false;
        if (digits.starts_with('+') || digits.starts_with('-')) {
            negative = digits.front() == '-';
            digits.erase(0, 1);
        }
        if (digits.empty() || digits.size() > 15 ||
            !std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; })) {
            return std::nullopt;
        }
        const std::int64_t value = std::stoll(digits);
        return negative ? -value : value;
    }
    return std::nullopt;
}

// load_client_pairing: the paired computer's display name, or nullopt when nothing usable is
// saved (no host or fingerprint, or a port outside 1..65535).
std::optional<std::string> remote_pairing_name(const Json& settings) {
    const Json raw = keys::remote_engine_client.read(settings);
    if (!raw.is_object()) {
        return std::nullopt;
    }
    const auto text = [&raw](const char* key) -> std::string {
        const auto found = raw.find(key);
        return found != raw.end() && found->is_string() ? found->get<std::string>() : std::string();
    };
    const std::string host = text("host");
    if (host.empty() || text("fingerprint").empty()) {
        return std::nullopt;
    }
    const std::optional<std::int64_t> port = pairing_port(raw);
    if (!port || *port < 1 || *port > 65535) {
        return std::nullopt;
    }
    const std::string name = text("host_name");
    return name.empty() ? host : name;
}

std::string_view device_label(std::string_view device) {
    if (device == "cuda") {
        return "GPU";
    }
    if (device == "cpu") {
        return "CPU";
    }
    return "Auto";
}

} // namespace

std::string voice_engine(const Json& settings) {
    return keys::selected_model.read(settings);
}

std::string voice_summary(const Json& settings) {
    return voice_summary(settings, local_today());
}

std::string voice_summary(const Json& settings, std::chrono::year_month_day today) {
    const std::string engine = voice_engine(settings);
    if (engine == "api") {
        const speech::SpeechEndpoint endpoint = speech::selected_endpoint(settings);
        return endpoint.name + " · " + speech::endpoint_model(endpoint, settings, today);
    }
    if (engine == "remote") {
        const std::optional<std::string> host = remote_pairing_name(settings);
        return host ? "Remote · " + *host : "Remote · not paired";
    }
    const std::string model = models::selected_model(settings, engine);
    const models::SpeechModel* found = models::model(model);
    return found != nullptr ? std::string(found->label) : model;
}

std::string voice_detail(const Json& settings, std::string device_info) {
    const std::string engine = voice_engine(settings);
    if (engine == "api") {
        const speech::SpeechEndpoint endpoint = speech::selected_endpoint(settings);
        if (endpoint.is_local()) {
            return "On this computer · " + endpoint.name;
        }
        return "Sent to " + speech::destination(endpoint) + " for transcription";
    }
    if (engine == "remote") {
        const std::optional<std::string> host = remote_pairing_name(settings);
        if (!host) {
            return "Pair with a host in Remote engine";
        }
        return "Sent to " + *host + " on your network";
    }
    if (device_info.empty()) {
        device_info = models::selected_device(settings, engine);
    }
    return "On this computer · " + device_info;
}

bool voice_is_cloud(const Json& settings) {
    if (voice_engine(settings) != "api") {
        return false;
    }
    return !speech::selected_endpoint(settings).is_local();
}

bool voice_is_remote(const Json& settings) {
    return voice_engine(settings) == "remote" || voice_is_cloud(settings);
}

std::string runtime_summary(const Json& settings, bool loaded) {
    const std::string device(device_label(models::selected_device(settings, "parakeet")));
    const std::string build = keys::fv_speech_runtime.read(settings);
    if (build.empty()) {
        return device + " · no runtime yet";
    }
    return std::format("{} · {} runtime {}", device, build == "cuda" ? "GPU" : "CPU",
                       loaded ? "loaded" : "not loaded");
}

std::string engine_caption(std::string_view backend) {
    if (models::backend(backend) != nullptr) {
        const models::SpeechModel* model = models::model(models::default_model(backend));
        return std::string(model != nullptr ? model->purpose : std::string_view()) +
               ". Runs locally.";
    }
    if (backend == "api") {
        return std::string(kApiEngineCaption);
    }
    if (backend == "remote") {
        return std::string(kRemoteEngineCaption);
    }
    return {};
}

std::string engine_filter(std::string_view backend) {
    return engine_is_local(backend) ? std::string(backend) : std::string();
}

bool engine_is_local(std::string_view backend) {
    return backend != "api" && backend != "remote";
}

std::string engine_inventory_count(std::string_view backend, const std::set<std::string>& cached) {
    if (!engine_is_local(backend)) {
        return {};
    }
    const models::SpeechBackend* known = models::backend(backend);
    if (known == nullptr) {
        throw std::out_of_range(std::format("Unknown speech backend '{}'", backend));
    }
    std::size_t total = 0;
    std::size_t present = 0;
    for (const models::SpeechModel& model : models::models()) {
        if (model.backend != backend) {
            continue;
        }
        ++total;
        if (cached.contains(std::string(model.key))) {
            ++present;
        }
    }
    return std::format("{} of {} {} {} on this computer", present, total, known->label,
                       total == 1 ? "model" : "models");
}

namespace {

// _check_engine_runtime's label for the component `resolve` names; what either throws reads as
// "could not be checked".
template <class Resolve> std::string installed_label(const models::Host& host, Resolve&& resolve) {
    try {
        const std::string component = resolve();
        const std::string name = models::component_copy(component).first;
        return std::format(" · {} {}.", name,
                           host.installed(component) ? "installed" : "not installed");
    } catch (const std::exception&) {
        return std::string(kRuntimeCheckFailed);
    }
}

} // namespace

std::string engine_runtime_label(std::string_view backend, std::string_view device,
                                 const models::Host& host) {
    return installed_label(host,
                           [&] { return models::resolve_runtime(backend, device, host).first; });
}

std::string engine_runtime_label(const Json& settings, std::string_view backend,
                                 std::string_view device, models::Host host) {
    if (backend != "parakeet") {
        return engine_runtime_label(backend, device, host);
    }
    if (const std::string chosen = keys::fv_runtime_dir.read(settings); !chosen.empty()) {
        // The chosen folder is whichever runtime the device needs, when it is one
        // (resolve_parakeet).
        host.is_installed = [folder = base_utf8::path_from_utf8(chosen)](std::string_view id) {
            return models::runtime_dir_serves(folder, id);
        };
    }
    const std::string build = keys::fv_speech_runtime.read(settings);
    if (build.empty()) {
        return engine_runtime_label(backend, device, host);
    }
    return installed_label(host, [&] { return models::runtime_id(backend, build, host); });
}

std::string engine_inventory_text(std::string_view backend, const std::set<std::string>& cached,
                                  std::string_view runtime_label) {
    std::string count = engine_inventory_count(backend, cached);
    if (count.empty()) {
        return count;
    }
    return count + std::string(runtime_label);
}

// --- Meeting voice ---

std::string speaker_id_label(std::string_view backend) {
    if (backend == "off") {
        return "Me / Others labels";
    }
    if (backend == "openai") {
        return "OpenAI speaker labels";
    }
    return "On-device speaker labels";
}

std::string meeting_language_label(std::string_view code) {
    for (const enums::Entry& entry : enums::labels(enums::EnumId::MeetingLanguage)) {
        if (entry.value.is_string() && entry.value.get_ref<const std::string&>() == code) {
            return entry.text;
        }
    }
    return std::string(code);
}

std::string meeting_model_label(const Json& settings) {
    const models::MeetingSpeech speech = models::resolve_meeting_speech(settings);
    if (speech.source == "remote") {
        return "Remote computer";
    }
    return models::meeting_model_label(speech.model);
}

std::string meeting_voice_summary(const Json& settings) {
    return meeting_model_label(settings) + " · " +
           meeting_language_label(keys::meeting_language.read(settings));
}

std::optional<std::string> meeting_preferred_host(const Json& settings) {
    try {
        return remote::meeting_route(settings).at("host_name").get<std::string>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string meeting_voice_detail(const Json& settings, const Clock& clock) {
    std::string detail =
        meeting_language_label(keys::meeting_language.read(settings)) + " · " +
        speaker_id_label(models::resolve_meeting_speaker_id_backend(settings, clock));
    if (models::resolve_meeting_asr_source(settings) == "remote") {
        detail += meeting_preferred_host(settings) ? " · audio sent to paired host"
                                                   : " · confirm paired host";
    }
    return detail;
}

std::string meeting_voice_detail(const Json& settings) {
    return meeting_voice_detail(settings, system_clock());
}

bool speaker_id_is_remote(const Json& settings, const Clock& clock) {
    return models::resolve_meeting_speaker_id_backend(settings, clock) == "openai";
}

bool speaker_id_is_remote(const Json& settings) {
    return speaker_id_is_remote(settings, system_clock());
}

std::string meeting_runtime_sentence(const Json& settings) {
    const models::MeetingSpeech speech = models::resolve_meeting_speech(settings);
    if (speech.source == "remote") {
        const auto host = meeting_preferred_host(settings);
        if (!host) {
            return "Confirm the paired host for meeting speech, or choose this computer.";
        }
        return *host +
               " is your preferred host for meeting speech. Each meeting asks before "
               "sending captured microphone and system audio there for transcription over an "
               "encrypted connection. Recordings and transcripts are saved on this computer.";
    }
    if (!speech.separate) {
        if (keys::selected_model.read(settings) == "remote") {
            return speech.note;
        }
        const std::string uses = !speech.note.empty()
                                     ? speech.note
                                     : "Meetings use " + models::meeting_model_label(speech.model) +
                                           ", the same as dictation.";
        return uses + " Change the dictation engine on Dictation → Voice model.";
    }
    const models::SpeechModel* entry = models::model(speech.model);
    if (entry == nullptr) {
        // resolve_meeting_speech only names catalog models; a name it does not know has no
        // device preference to report.
        return {};
    }
    return std::format("Uses {} with its {} device preference. Set that engine's device on "
                       "Dictation → Voice model. Install its runtime and model in Downloads.",
                       entry->label,
                       device_label(models::selected_device(settings, entry->backend)));
}

} // namespace fv
