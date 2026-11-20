#pragma once

#include <string>
#include <string_view>

#include "automation/action_model.h"
#include "base/json.h"
#include "dictation/job.h"
#include "golden/golden.h"
#include "pyport/json.h"

// Shared by the golden replays of tests/automation: building C++ values from the specs the case
// files (tools/parity/cases/*.py) pass to OpenWhisper.
namespace fv::automation::test {

// A string argument; null (Python's None, read through `or ""`) is "".
inline std::string text(const Json& value) {
    return value.is_string() ? value.get<std::string>() : std::string();
}

// The expected JSON with OpenWhisper's names as FioriVoce's.
inline Json rebranded(const Json& expected) {
    return py::loads(golden::rebrand(py::dumps(expected)));
}

// Whether the vectors were made on this platform, for cases whose answer depends on os.path or
// the platform's shortcut names.
inline bool same_platform(std::string_view module) {
    const std::string platform = golden::load(module).at("pins").value("platform", "");
#ifdef _WIN32
    return platform == "win32";
#else
    return platform == "linux";
#endif
}

// A DictationJob from a dict of its fields, or nullptr for null.
inline const dictation::DictationJob* job_from(const Json& spec, dictation::DictationJob& storage) {
    if (spec.is_null()) {
        return nullptr;
    }
    storage = dictation::DictationJob{};
    storage.id = spec.value("id", storage.id);
    storage.mode = dictation::job_mode_from(spec.value("mode", "dictation"))
                       .value_or(dictation::JobMode::Dictation);
    storage.action_id = spec.value("action_id", "");
    storage.source = spec.value("source", "");
    storage.route = spec.value("route", "");
    storage.routed = spec.value("routed", false);
    storage.changed_by_program = spec.value("changed_by_program", false);
    return &storage;
}

// An Action from a dict of Action fields, with the dataclass's defaults.
inline Action action_from(const Json& spec) {
    Action action;
    action.id = text(spec.value("id", Json()));
    action.name = text(spec.value("name", Json()));
    action.kind = text(spec.value("kind", Json()));
    action.params = spec.value("params", Json::object());
    action.hotkey = spec.value("hotkey", "");
    action.shaping = spec.value("shaping", std::string(SHAPING_CLEANUP));
    for (const Json& name : spec.value("spoken_names", Json::array())) {
        action.spoken_names.push_back(name.get<std::string>());
    }
    action.description = spec.value("description", "");
    action.enabled = spec.value("enabled", true);
    return action;
}

} // namespace fv::automation::test
