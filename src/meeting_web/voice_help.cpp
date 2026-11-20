#include "meeting_web/voice_help.h"

#include <algorithm>
#include <string>

#include "pyport/text.h"
#include "settings/keys.h"

namespace fv::meeting_web {

namespace {

// The label a configured name is shown with.
std::string display_name(const std::string& name) {
    if (name == "note taker") {
        return "Note taker";
    }
    for (const AppIdentity* app : {&identity(), &legacy_identity()}) {
        if (name == app->wake_name || name == app->wake_name_spaced) {
            return std::string(app->product);
        }
    }
    return py::capitalize(name);
}

bool contains(const std::vector<std::string>& names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

std::vector<std::string> default_voice_command_names(const AppIdentity& defaults_from) {
    return {"note taker", "notetaker", "assistant", std::string(defaults_from.wake_name),
            std::string(defaults_from.wake_name_spaced)};
}

std::vector<std::string> resolve_voice_command_names(const Json& settings,
                                                     const AppIdentity& defaults_from) {
    const Json raw = keys::typesafe_voice_command_names.read(settings);
    if (!raw.is_array()) {
        return default_voice_command_names(defaults_from);
    }
    std::vector<std::string> names;
    for (const auto& item : raw) {
        if (!item.is_string()) {
            continue;
        }
        const auto& text = item.get_ref<const std::string&>();
        if (py::strip(text).empty()) {
            continue;
        }
        std::string joined;
        for (const auto& word : py::split(py::lower(py::strip(text)))) {
            if (!joined.empty()) {
                joined += ' ';
            }
            joined += word;
        }
        names.push_back(std::move(joined));
    }
    return names.empty() ? default_voice_command_names(defaults_from) : names;
}

Json voice_command_guide(const Json& settings, const AppIdentity& defaults_from) {
    const auto configured = resolve_voice_command_names(settings, defaults_from);
    std::vector<std::string> names;
    for (const auto& name : configured) {
        if (name == "notetaker" && contains(configured, "note taker")) {
            continue;
        }
        std::string label = display_name(name);
        if (!contains(names, label)) {
            names.push_back(std::move(label));
        }
    }
    if (const auto assistant = std::find(names.begin(), names.end(), "Assistant");
        assistant != names.end()) {
        names.erase(assistant);
        names.insert(names.begin(), "Assistant");
    }
    const std::string primary = names.front();
    auto example = [&](const char* label, const std::string& phrase) {
        return Json{{"label", label}, {"phrase", primary + ", " + phrase}};
    };
    return Json{
        {"names", names},
        {"primary", primary},
        {"examples", Json::array({
                         example("Take a note", "note that the launch is Friday."),
                         example("Mark a decision", "mark that as a decision."),
                         example("Add an action", "mark that as an action item."),
                         example("Get a recap", "recap the decisions so far."),
                         example("Change topic", "new topic: budget."),
                         example("Correct a term", "replace Acme with Acme Labs."),
                     })},
    };
}

} // namespace fv::meeting_web
