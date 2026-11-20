#include "speech/dictation_language.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <utility>

#include "pyport/text.h"
#include "settings/enums.h"
#include "settings/keys.h"
#include "speech/languages.h"

namespace fv::speech {

namespace {

constexpr std::string_view kAuto = "auto"; // MeetingLanguage.AUTO
constexpr std::string_view kRemote = "remote";
std::mutex runtime_mutex;
std::map<std::string, RemoteVoiceCapabilities, std::less<>> remote_runtimes;

bool contains(const std::vector<std::string>& codes, std::string_view code) {
    return std::ranges::find(codes, code) != codes.end();
}

// _LABELS: LANGUAGE_LABELS, then MeetingLanguage.CHOICES over them ("zh" is "Chinese", "auto"
// is "Detect automatically").
const std::map<std::string, std::string, std::less<>>& labels() {
    static const auto table = [] {
        std::map<std::string, std::string, std::less<>> made;
        for (const auto& [code, text] : language_labels()) {
            made[std::string(code)] = std::string(text);
        }
        for (const enums::Entry& entry : enums::labels(enums::EnumId::MeetingLanguage)) {
            made[entry.value.get<std::string>()] = entry.text;
        }
        return made;
    }();
    return table;
}

// _WHISPER_CODES: MeetingLanguage.ALL without auto, in order.
const std::vector<std::string>& whisper_codes() {
    static const auto codes = [] {
        std::vector<std::string> made;
        for (const Json& value : enums::values(enums::EnumId::MeetingLanguage)) {
            const auto code = value.get<std::string>();
            if (code != kAuto) {
                made.push_back(code);
            }
        }
        return made;
    }();
    return codes;
}

void set_active_language(Json& settings, std::string_view code) {
    settings[std::string(keys::dictation_active_language.key())] = std::string(code);
}

} // namespace

std::string engine(const Json& settings) {
    return keys::selected_model.read(settings);
}

std::optional<RemoteVoiceCapabilities> remote_voice_capabilities(const Json& settings) {
    if (engine(settings) != kRemote) {
        return std::nullopt;
    }
    const Json pairing = keys::remote_engine_client.read(settings);
    const auto fingerprint = pairing.find("fingerprint");
    if (fingerprint == pairing.end() || !fingerprint->is_string()) {
        return std::nullopt;
    }
    std::lock_guard lock(runtime_mutex);
    const auto found = remote_runtimes.find(fingerprint->get_ref<const std::string&>());
    return found != remote_runtimes.end() ? std::optional(found->second) : std::nullopt;
}

void note_remote_runtime(std::string_view fingerprint, RemoteVoiceCapabilities runtime) {
    if (fingerprint.empty()) {
        return;
    }
    std::lock_guard lock(runtime_mutex);
    remote_runtimes.insert_or_assign(std::string(fingerprint), std::move(runtime));
}

void forget_remote_runtime(std::string_view fingerprint) {
    std::lock_guard lock(runtime_mutex);
    const auto found = remote_runtimes.find(fingerprint);
    if (found != remote_runtimes.end()) {
        remote_runtimes.erase(found);
    }
}

std::vector<std::string> accepted_languages(const Json& settings) {
    const std::string backend = engine(settings);
    if (backend == kRemote) {
        const auto runtime = remote_voice_capabilities(settings);
        // An OpenWhisper host running Whisper reports no list (or an empty one): it takes every
        // Whisper language, as one that has not said which family it runs does.
        if (!runtime || runtime->family.empty() || runtime->family == "local_whisper") {
            return whisper_codes();
        }
        std::vector<std::string> accepted;
        if (runtime->languages) {
            for (const auto& code : *runtime->languages) {
                if (code != kAuto && labels().contains(code) && !contains(accepted, code)) {
                    accepted.push_back(code);
                }
            }
            return accepted;
        }
        if (runtime->family == "parakeet") {
            for (const auto code : speech::language_choices(std::string_view(runtime->family))) {
                if (code != kAuto) {
                    accepted.emplace_back(code);
                }
            }
            return accepted;
        }
        return whisper_codes();
    }
    if (backend == "api") {
        return whisper_codes();
    }
    std::vector<std::string> accepted;
    for (const std::string_view code : speech::language_choices(std::string_view(backend))) {
        if (code != kAuto) {
            accepted.emplace_back(code);
        }
    }
    return accepted;
}

std::string single_language_reason(const Json& settings) {
    if (accepted_languages(settings).size() > 1) {
        return "";
    }
    if (engine(settings) == kRemote) {
        return "The other computer's engine uses one language.";
    }
    return "This engine uses one language.";
}

std::vector<std::string> chosen_languages(const Json& settings) {
    const Json raw = keys::dictation_languages.read(settings);
    std::vector<std::string> chosen;
    for (const Json& item : raw) {
        if (!item.is_string()) {
            continue;
        }
        const auto code = item.get<std::string>();
        if (labels().contains(code) && code != kAuto && !contains(chosen, code)) {
            chosen.push_back(code);
        }
    }
    return chosen;
}

std::vector<std::string> language_choices(const Json& settings) {
    const std::vector<std::string> accepted = accepted_languages(settings);
    std::vector<std::string> choices;
    for (std::string& code : chosen_languages(settings)) {
        if (contains(accepted, code)) {
            choices.push_back(std::move(code));
        }
    }
    return choices;
}

std::string job_language(const Json& settings) {
    const std::string active = keys::dictation_active_language.read(settings);
    return contains(language_choices(settings), active) ? active : "";
}

std::string engine_language(const Json& settings) {
    const std::string backend = engine(settings);
    if (backend == kRemote) {
        const auto runtime = remote_voice_capabilities(settings);
        if (runtime && labels().contains(runtime->own_language)) {
            return runtime->own_language;
        }
        const auto accepted = accepted_languages(settings);
        return runtime && accepted.size() == 1 ? accepted.front() : std::string(kAuto);
    }
    if (backend == "api") {
        const std::vector<std::string> accepted = accepted_languages(settings);
        return accepted.size() == 1 ? accepted.front() : std::string(kAuto);
    }
    return selected_language(backend, keys::local_asr_language.read(settings));
}

std::string current_language(const Json& settings) {
    std::string job = job_language(settings);
    return job.empty() ? engine_language(settings) : job;
}

void set_active(Json& settings, std::string_view code) {
    if (contains(language_choices(settings), code)) {
        set_active_language(settings, code);
    }
}

void set_chosen(Json& settings, const std::vector<std::string>& codes) {
    Json list = Json::array();
    for (const std::string& code : codes) {
        list.push_back(code);
    }
    Json wrapped = Json::object();
    wrapped[std::string(keys::dictation_languages.key())] = std::move(list);
    Json chosen = Json::array();
    for (const std::string& code : chosen_languages(wrapped)) {
        chosen.push_back(code);
    }
    settings[std::string(keys::dictation_languages.key())] = std::move(chosen);

    const std::vector<std::string> choices = language_choices(settings);
    // Repairs what is stored, so a malformed value counts as not a choice.
    const auto stored = keys::dictation_active_language.stored(settings);
    if (!stored || !contains(choices, *stored)) {
        set_active_language(settings, choices.empty() ? std::string() : choices.front());
    }
}

void add_chosen(Json& settings, std::string_view code) {
    std::vector<std::string> chosen = chosen_languages(settings);
    const std::string own = job_language(settings).empty() ? engine_language(settings) : "";
    const std::string keep = contains(accepted_languages(settings), own) ? own : "";
    if (!keep.empty() && !contains(chosen, keep) && keep != code) {
        chosen.push_back(keep);
    }
    chosen.emplace_back(code);
    set_chosen(settings, chosen);
    if (!keep.empty()) {
        set_active(settings, keep);
    }
}

void cycle(Json& settings) {
    const std::vector<std::string> choices = language_choices(settings);
    if (choices.size() < 2) {
        return;
    }
    const std::string current = current_language(settings);
    const auto found = std::ranges::find(choices, current);
    const std::string& following =
        found == choices.end()
            ? choices.front()
            : choices[(static_cast<std::size_t>(found - choices.begin()) + 1) % choices.size()];
    set_active_language(settings, following);
}

std::string label(std::string_view code) {
    const auto found = labels().find(code);
    return found == labels().end() ? std::string(code) : found->second;
}

std::string short_label(std::string_view code) {
    return code == kAuto ? "AUTO" : py::upper(code);
}

} // namespace fv::speech
