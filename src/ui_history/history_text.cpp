#include "ui_history/history_text.h"

#include <array>
#include <utility>

#include "history/manager.h"
#include "pyport/text.h"

namespace fv {
namespace {

using Pair = std::pair<std::string_view, std::string_view>;

// _MODEL_DISPLAY_NAMES.
constexpr std::array<Pair, 5> kModelNames{{
    {"local_whisper", "Local"},
    {"api", "API"},
    {"api_whisper", "API"},
    {"api_gpt4o", "GPT-4o"},
    {"api_gpt4o_mini", "GPT-4o Mini"},
}};

// _CLEANUP_PROVIDER_DISPLAY_NAMES.
constexpr std::array<Pair, 2> kProviderNames{{
    {"openai", "OpenAI"},
    {"openrouter", "OpenRouter"},
}};

// _CLEANUP_LEVEL_LABELS.
constexpr std::array<Pair, 5> kLevelLabels{{
    {"light", "Light"},
    {"medium", "Medium"},
    {"high", "High"},
    {"custom", "Custom"},
    {"profile", "Profile"},
}};

// text_llm.profile_display_name's agent_names.
constexpr std::array<Pair, 3> kAgentNames{{
    {"claude_code", "Claude Code"},
    {"codex", "Codex"},
    {"opencode_cli", "OpenCode"},
}};

// text_llm.builtin_profiles(): OpenAI, OpenRouter, LOCAL_SERVER_NAMES, then the hosted ones.
constexpr std::array<Pair, 10> kBuiltinProfiles{{
    {"openai", "OpenAI"},
    {"openrouter", "OpenRouter"},
    {"ollama", "Ollama"},
    {"lm_studio", "LM Studio"},
    {"vllm", "vLLM"},
    {"llama_cpp", "llama.cpp"},
    {"localai", "LocalAI"},
    {"groq", "Groq"},
    {"opencode_go", "OpenCode Go"},
    {"opencode_zen", "OpenCode Zen"},
}};

// LABELLED_KINDS and the source-name prefixes of services/history_calendar.py.
constexpr std::string_view kCommandKind = "command";
constexpr std::string_view kTransformKind = "transform";
constexpr std::string_view kActionKind = "action";
constexpr std::string_view kWalkthroughKind = "walkthrough";
constexpr std::string_view kTransformPrefix = "Transform · ";
constexpr std::string_view kActionPrefix = "Action · ";

// Python's `a · b`, with the middle dot.
constexpr std::string_view kDot = " · ";

// The FioriVoce engine whose stored model info names its own model.
constexpr std::string_view kParakeetBase = "parakeet";

template <std::size_t N>
std::optional<std::string_view> lookup(const std::array<Pair, N>& table, std::string_view key) {
    for (const auto& [name, value] : table) {
        if (name == key) {
            return value;
        }
    }
    return std::nullopt;
}

// Python truthiness of an optional str.
bool truthy(const std::optional<std::string>& value) {
    return value.has_value() && !value->empty();
}

std::string joined(std::string_view left, std::string_view right) {
    std::string out(left);
    out += kDot;
    out += right;
    return out;
}

// classify_source(...)[1] for the labelled kinds: name.removeprefix(prefix).strip().
std::string prefixed_label(const std::string& name, std::string_view prefix,
                           std::string_view bare) {
    std::string_view rest = name;
    if (rest.starts_with(prefix)) {
        rest.remove_prefix(prefix.size());
    }
    const std::string stripped = py::strip(rest);
    if (stripped.empty()) {
        return std::string(bare);
    }
    return std::string(prefix) + stripped;
}

void put(Json& object, const char* key, const std::optional<std::string>& value) {
    object[key] = value ? Json(*value) : Json(nullptr);
}

void put(Json& object, const char* key, const std::optional<double>& value) {
    object[key] = value ? Json(*value) : Json(nullptr);
}

} // namespace

std::string format_model_name(std::string_view model) {
    const std::size_t open = model.find('(');
    const std::string base = py::strip(model.substr(0, open));
    std::string_view detail =
        open == std::string_view::npos ? std::string_view{} : model.substr(open + 1);
    while (detail.ends_with(')')) {
        detail.remove_suffix(1);
    }
    detail = detail.substr(0, detail.find('|'));
    const std::string shown_detail = py::strip(detail);
    if (base == kParakeetBase) {
        return shown_detail.empty() ? std::string("Parakeet") : shown_detail;
    }
    const std::string name(lookup(kModelNames, base).value_or(base));
    return shown_detail.empty() ? name : joined(name, shown_detail);
}

std::string cleanup_level_label(const HistoryEntry& entry) {
    if (!truthy(entry.cleanup_level)) {
        return {};
    }
    return std::string(lookup(kLevelLabels, py::lower(*entry.cleanup_level)).value_or(""));
}

std::string profile_display_name(std::string_view profile_id) {
    if (const auto agent = lookup(kAgentNames, profile_id)) {
        return std::string(*agent);
    }
    const std::string stripped = py::strip(profile_id);
    if (!stripped.empty()) {
        if (const auto builtin = lookup(kBuiltinProfiles, stripped)) {
            return std::string(*builtin);
        }
    }
    return stripped;
}

std::string cleanup_provider_name(const HistoryEntry& entry) {
    const std::string id = entry.cleanup_provider.value_or("");
    if (const auto known = lookup(kProviderNames, id)) {
        return std::string(*known);
    }
    if (id.empty()) {
        return {};
    }
    std::string name = profile_display_name(id);
    return name.empty() ? id : name;
}

std::string format_cleanup_info(const HistoryEntry& entry) {
    const std::string level = cleanup_level_label(entry);
    if (!truthy(entry.cleanup_model)) {
        return level.empty() ? std::string("Cleaned") : level;
    }
    const std::string& model = *entry.cleanup_model;
    if (!level.empty()) {
        return joined(level, model);
    }
    const std::string provider = cleanup_provider_name(entry);
    return provider.empty() ? model : joined(provider, model);
}

std::string cleanup_tooltip(const HistoryEntry& entry) {
    if (!truthy(entry.cleanup_model)) {
        return "Transcript was cleaned (model not recorded)";
    }
    const std::string provider = cleanup_provider_name(entry);
    const std::string model =
        provider.empty() ? *entry.cleanup_model : joined(provider, *entry.cleanup_model);
    const std::string level = cleanup_level_label(entry);
    std::string tooltip = "Transcript cleaned with " + model;
    if (!level.empty()) {
        tooltip += " (" + level + ")";
    }
    return tooltip;
}

bool entry_was_cleaned(const HistoryEntry& entry) {
    return (truthy(entry.cleanup_model) || truthy(entry.raw_text)) &&
           entry_version(entry) != ORIGINAL_VERSION;
}

std::string kind_label(const HistoryEntry& entry) {
    if (!entry.entry_kind) {
        return {};
    }
    const std::string& kind = *entry.entry_kind;
    if (kind == FAILED_RECORDING_KIND) {
        return "Transcription failed";
    }
    const std::string name = py::strip(entry.source_name.value_or(""));
    if (kind == kCommandKind) {
        return "Command";
    }
    if (kind == kWalkthroughKind) {
        return "Walkthrough";
    }
    if (kind == kTransformKind) {
        return prefixed_label(name, kTransformPrefix, "Transform");
    }
    if (kind == kActionKind) {
        return prefixed_label(name, kActionPrefix, "Action");
    }
    return {};
}

std::string failed_recording_hint(bool has_audio) {
    if (has_audio) {
        return "Audio saved. Retry with the currently selected speech engine.";
    }
    return "The saved audio is no longer available.";
}

LocationChip location_chip(const HistoryEntry& entry) {
    if (truthy(entry.stored_on)) {
        return {"On " + *entry.stored_on, "Kept on " + *entry.stored_on + "; opened from there"};
    }
    if (truthy(entry.also_on)) {
        return {"Also on " + *entry.also_on, "Kept here and on " + *entry.also_on};
    }
    if (truthy(entry.origin_device_name)) {
        return {"From " + *entry.origin_device_name,
                "Kept here for " + *entry.origin_device_name + ", a paired computer"};
    }
    return {};
}

Json card_fingerprint(const HistoryEntry& entry, std::optional<bool> audio_available) {
    Json value = Json::object();
    value["id"] = entry.id;
    value["text"] = entry.text;
    put(value, "raw_text", entry.raw_text);
    value["timestamp"] = entry.timestamp;
    value["model"] = entry.model;
    put(value, "audio_file", entry.audio_file);
    put(value, "transcription_time", entry.transcription_time);
    put(value, "audio_duration", entry.audio_duration);
    value["file_size"] = entry.file_size ? Json(*entry.file_size) : Json(nullptr);
    put(value, "cleanup_provider", entry.cleanup_provider);
    put(value, "cleanup_model", entry.cleanup_model);
    put(value, "source_name", entry.source_name);
    put(value, "title", entry.title);
    put(value, "origin_device_id", entry.origin_device_id);
    put(value, "origin_device_name", entry.origin_device_name);
    put(value, "app_id", entry.app_id);
    put(value, "app_name", entry.app_name);
    put(value, "app_category", entry.app_category);
    put(value, "cleanup_level", entry.cleanup_level);
    put(value, "entry_kind", entry.entry_kind);
    put(value, "cleaned_text", entry.cleaned_text);
    put(value, "language", entry.language);
    put(value, "stored_on", entry.stored_on);
    put(value, "also_on", entry.also_on);
    value["remote_audio"] = entry.remote_audio;
    value["cleanup_display"] = format_cleanup_info(entry);
    if (truthy(entry.audio_file) && !truthy(entry.stored_on) && audio_available) {
        value["audio_available"] = *audio_available;
    }
    return value;
}

} // namespace fv
