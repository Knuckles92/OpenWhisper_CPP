#include "text/personalize_copy.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <set>
#include <stdexcept>

#include "base/identity.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "text/app_styles.h"
#include "text/cleanup_prompts.h"
#include "text/detail/prompt_data.h"
#include "text/dictionary.h"
#include "text/recognition.h"
#include "text/transforms.h"

namespace fv::text {

namespace {

// _join: "a", "a and b", "a, b and c".
std::string join_names(const std::vector<std::string>& names) {
    if (names.empty()) {
        throw std::out_of_range("join_names: no names");
    }
    if (names.size() == 1) {
        return names.front();
    }
    std::string out;
    for (std::size_t i = 0; i + 1 < names.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += names[i];
    }
    return out + " and " + names.back();
}

// _PLACES: how home_copy names each category.
std::string place(std::string_view category) {
    if (category == app_styles::EMAIL) {
        return "email";
    }
    if (category == app_styles::WORK) {
        return "work messages";
    }
    if (category == app_styles::PERSONAL) {
        return "personal messages";
    }
    return "other apps";
}

// format(count, ","): thousands separated by commas.
std::string grouped(std::size_t count) {
    std::string digits = std::to_string(count);
    for (std::ptrdiff_t at = static_cast<std::ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<std::size_t>(at), ",");
    }
    return digits;
}

} // namespace

namespace styles_page {

bool cleanup_on(const Json& settings) {
    return keys::transcript_cleanup_enabled.read(detail::or_empty(settings));
}

bool reads_text_near_cursor(const Json& settings, bool text_reading_supported) {
    const Json& values = detail::or_empty(settings);
    return keys::app_context_enabled.read(values) && keys::app_context_read_text.read(values) &&
           text_reading_supported;
}

std::string styles_blocker(const Json& settings) {
    if (!cleanup_on(settings)) {
        return "Styles need AI cleanup, which is off.";
    }
    if (!keys::app_context_enabled.read(detail::or_empty(settings))) {
        return "Styles need Know which app I'm dictating into.";
    }
    return "";
}

std::string rail_value(const Json& settings) {
    const Json& values = detail::or_empty(settings);
    if (!keys::app_context_enabled.read(values) || !keys::app_styles_enabled.read(values)) {
        return "Off";
    }
    if (!cleanup_on(values)) {
        return "Needs AI cleanup";
    }
    std::size_t relaxed = 0;
    const auto tones = app_styles::resolve_tones(values);
    for (const auto& [category, tone] : tones.items()) {
        relaxed += tone.get<std::string>() != app_styles::FORMAL ? 1 : 0;
    }
    return relaxed > 0 ? "On · " + std::to_string(relaxed) + " casual" : "On · All formal";
}

std::string home_copy(const Json& settings) {
    const Json tones = app_styles::resolve_tones(settings);
    const auto has = [](const std::vector<std::string_view>& list, std::string_view item) {
        return std::find(list.begin(), list.end(), item) != list.end();
    };
    std::vector<std::string> parts;
    for (const auto& [tone, label] : TONE_LABELS) {
        std::vector<std::string_view> categories;
        for (const std::string_view category : app_styles::CATEGORIES) {
            if (tones.at(std::string(category)).get<std::string>() == tone) {
                categories.push_back(category);
            }
        }
        if (categories.size() == app_styles::CATEGORIES.size()) {
            if (tone == app_styles::FORMAL) {
                return "Formal in every app. Change tones in Styles.";
            }
            return std::string(label) + " in every app.";
        }
        std::vector<std::string> places;
        if (has(categories, app_styles::WORK) && has(categories, app_styles::PERSONAL)) {
            for (const std::string_view category : categories) {
                if (category != app_styles::WORK && category != app_styles::PERSONAL) {
                    places.push_back(place(category));
                }
            }
            const std::size_t at = has(categories, app_styles::EMAIL) ? 1 : 0;
            places.insert(places.begin() + static_cast<std::ptrdiff_t>(std::min(at, places.size())),
                          "chat");
        } else {
            for (const std::string_view category : categories) {
                places.push_back(place(category));
            }
        }
        if (!places.empty()) {
            parts.push_back(py::lower(label) + " for " + join_names(places));
        }
    }
    std::string sentence;
    for (const std::string& part : parts) {
        if (!sentence.empty()) {
            sentence += ", ";
        }
        sentence += part;
    }
    // Every part starts with an ASCII tone label, so upper-casing its first byte is
    // sentence[0].upper().
    if (!sentence.empty()) {
        sentence.front() =
            static_cast<char>(std::toupper(static_cast<unsigned char>(sentence.front())));
    }
    return sentence + ".";
}

std::string apps_summary(std::string_view category,
                         const std::vector<focus::catalog::Override>& overrides) {
    std::vector<std::pair<std::string, std::string>> moved; // casefolded match -> category
    for (const auto& [match, chosen] : overrides) {
        const std::string key = py::casefold(match);
        bool replaced = false;
        for (auto& [existing, value] : moved) {
            if (existing == key) {
                value = chosen;
                replaced = true;
            }
        }
        if (!replaced) {
            moved.emplace_back(key, chosen);
        }
    }
    std::vector<std::string> names;
    std::set<std::string> taken;
    for (const auto& [match, chosen] : overrides) {
        if (chosen == category) {
            names.push_back(match);
            taken.insert(py::casefold(match));
        }
    }
    const std::vector<std::string>* featured = nullptr;
    for (const auto& [name, list] : focus::catalog::featured()) {
        if (name == category) {
            featured = &list;
        }
    }
    if (featured == nullptr) {
        throw std::out_of_range("No featured apps for " + std::string(category));
    }
    for (const std::string& name : *featured) {
        const std::string key = py::casefold(name);
        if (taken.contains(key)) {
            continue;
        }
        std::string_view chosen = category;
        for (const auto& [existing, value] : moved) {
            if (existing == key) {
                chosen = value;
            }
        }
        if (chosen == category) {
            names.push_back(name);
        }
    }
    if (names.size() > 4) {
        names.resize(4);
    }
    if (category == app_styles::OTHER) {
        return names.empty() ? std::string("Everything else.")
                             : "Everything else, like " + join_names(names) + ".";
    }
    if (names.empty()) {
        return "Apps you add below.";
    }
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
    }
    return out + " and more.";
}

std::string privacy_note(const Json& settings, bool text_reading_supported,
                         const ProviderInfo& provider) {
    if (!text_reading_supported) {
        return "Works on Windows for now. Here, " + std::string(identity().product) +
               " only knows which app you're in.";
    }
    if (!keys::app_context_enabled.read(detail::or_empty(settings))) {
        return "Turn on Know which app I'm dictating into first.";
    }
    if (!cleanup_on(settings)) {
        return "AI cleanup is off, so it only fixes spacing and capitals on this computer. "
               "Never saved.";
    }
    const std::string name = provider.name.empty() ? "your provider" : provider.name;
    if (provider.local) {
        return "Used only by your AI cleanup provider (" + name +
               "), on this computer. Never saved.";
    }
    return "Sent only to your AI cleanup provider (" + name +
           ") with the dictation. Never saved, and never sent to a speech engine.";
}

std::vector<std::string> excluded(const Json& settings) {
    std::vector<std::string> values;
    std::set<std::string> seen;
    for (const Json& value : keys::app_context_excluded_apps.read(detail::or_empty(settings))) {
        const std::string name =
            value.is_string() ? py::strip(value.get_ref<const std::string&>()) : std::string();
        if (!name.empty() && seen.insert(py::casefold(name)).second) {
            values.push_back(name);
        }
    }
    return values;
}

} // namespace styles_page

namespace dictionary_page {

namespace {

bool cleanup_on(const Json& settings) {
    return resolve_level(settings) != CleanupLevel::None;
}

} // namespace

EngineInfo engine_info(const Json& settings, const std::optional<Endpoint>& endpoint,
                       const std::optional<RemoteBackend>& remote) {
    const std::string engine = keys::selected_model.read(detail::or_empty(settings));
    EngineInfo info{engine, "", "", engine_support(engine)};
    // local_asr.catalog.BACKENDS, for the families keys::selected_model can hold.
    if (engine == "parakeet") {
        info.name = "Parakeet";
    }
    if (engine == "api" && endpoint) {
        info.name = endpoint->name;
        // Words for a server on this computer stay here with the audio.
        info.destination = endpoint->local ? std::string() : endpoint->name;
    }
    if (engine == "remote" && remote) {
        info.support = remote->support;
        info.name = remote->name;
        info.destination = remote->host_name;
    }
    if (info.name.empty()) {
        return {engine, "", "", ""};
    }
    return info;
}

bool cannot_steer(const EngineInfo& engine) {
    return engine.support == SUPPORT_AFTER;
}

bool sounds_like_only(const Json& settings, const EngineInfo& engine) {
    if (engine.support.empty() || cleanup_on(settings)) {
        return false;
    }
    return engine.support != SUPPORT_MODEL ||
           !keys::dictionary_steer_recognition.read(detail::or_empty(settings));
}

std::string engine_line(const Json& settings, const EngineInfo& engine) {
    if (engine.support.empty()) {
        return "Engines that take hints also listen for your words. Every engine fixes "
               "spellings after recognition.";
    }
    if (engine.support != SUPPORT_MODEL) {
        if (cleanup_on(settings)) {
            return "Your engine (" + engine.name +
                   ") can't be steered. AI cleanup and Sounds like spellings fix your words "
                   "after recognition.";
        }
        return "Your engine (" + engine.name +
               ") can't be steered, and AI cleanup is off, so only Sounds like spellings are "
               "fixed.";
    }
    std::string line =
        "Your engine (" + engine.name + ") also steers the speech model toward your words.";
    if (!engine.destination.empty()) {
        line += " They're sent to " + engine.destination + " with your audio.";
    }
    return line;
}

std::string rail_value(const Json& settings) {
    return summary(settings);
}

std::string home_detail(const Json& settings, const EngineInfo& engine) {
    const bool limited = sounds_like_only(settings, engine);
    if (!load_dictionary(settings).empty()) {
        std::string count = summary(settings);
        return limited ? count + " · Only Sounds like spellings are fixed while AI cleanup is off."
                       : count;
    }
    if (limited) {
        return "Names and terms to get right. With AI cleanup off, each needs a Sounds like "
               "spelling.";
    }
    return "Names and terms dictation should always get right.";
}

} // namespace dictionary_page

namespace commands_page {

std::string command_hotkey(const Json& settings) {
    const Json saved = keys::hotkeys.read(detail::or_empty(settings));
    if (!saved.is_object()) {
        return "";
    }
    const auto found = saved.find("command_mode");
    return found != saved.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::string rail_value(const Json& settings) {
    const std::size_t count = load_transforms(settings).size();
    const std::string shortcut = command_hotkey(settings).empty() ? "No shortcut" : "Shortcut set";
    return shortcut + " · " + std::to_string(count) + " transform" + (count == 1 ? "" : "s");
}

std::string command_mode_hint(const Json& settings) {
    if (keys::recording_trigger_mode.read(detail::or_empty(settings)) == "push_hold") {
        return "Hold the shortcut while you speak, then let go.";
    }
    return "Press the shortcut, speak, then press it again.";
}

} // namespace commands_page

namespace snippets_page {

std::string count(const Json& settings) {
    const std::size_t count = load_snippets(settings).size();
    if (count == 0) {
        return "";
    }
    return std::to_string(count) + " snippet" + (count == 1 ? "" : "s");
}

std::string rail_value(const Json& settings) {
    if (!keys::snippets_enabled.read(detail::or_empty(settings))) {
        return "Off";
    }
    std::string value = count(settings);
    return value.empty() ? std::string("None yet") : value;
}

std::string preview(const Snippet& draft) {
    if (draft.trigger.empty()) {
        return "Say the trigger on its own or inside a sentence.";
    }
    if (py::strip(draft.text).empty()) {
        return "Say “" + draft.trigger + "” → add the text it inserts.";
    }
    const std::size_t count = py::len(plain_text(draft));
    const std::string noun = count == 1 ? "character" : "characters";
    const std::string styled = draft.formatted ? " with formatting" : "";
    return "Say “" + draft.trigger + "” → inserts " + grouped(count) + " " + noun + styled;
}

} // namespace snippets_page

namespace cleanup_rules_page {

std::string rule_count_label(std::int64_t rule_count) {
    if (rule_count == 0) {
        return "No rules";
    }
    return std::to_string(rule_count) + " rule" + (rule_count == 1 ? "" : "s");
}

} // namespace cleanup_rules_page

} // namespace fv::text
