#include "backup/detail/action_rows.h"

#include <algorithm>
#include <array>
#include <exception>
#include <optional>
#include <set>
#include <utility>

#include "hotkeys/conflicts.h"
#include "pyport/text.h"
#include "settings/keys.h"

namespace fv::backup::detail {

namespace {

constexpr std::array<std::string_view, 7> kKinds = {kOpenLink, kAgent,     kChannel, kWebhook,
                                                    kProgram,  kClipboard, kScripts};
constexpr std::string_view kShapingCleanup = "cleanup";
// _coerce keeps at most this many strings of a list parameter.
constexpr std::size_t kMaxListItems = 32;

// PARAMS: each kind's parameters with their defaults, in order; a default's type is the type
// the parameter accepts.
const Json& kind_params(std::string_view kind) {
    static const Json table = [] {
        Json t = Json::object();
        t[std::string(kOpenLink)] = Json{{"template", ""}};
        t[std::string(kAgent)] = Json{{"agent", "claude_code"},
                                      {"mode", "handoff"},
                                      {"surface", "auto"},
                                      {"folder", ""},
                                      {"new_folder", false},
                                      {"include_selection", false},
                                      {"model", ""},
                                      {"output", "scratchpad"}};
        t[std::string(kChannel)] = Json::object();
        t[std::string(kWebhook)] = Json{{"url", ""}, {"output", "none"}};
        t[std::string(kProgram)] =
            Json{{"program", ""}, {"args", Json::array()}, {"output", "none"}, {"timeout_s", 30}};
        t[std::string(kClipboard)] = Json::object();
        t[std::string(kScripts)] = Json::object();
        return t;
    }();
    return table.at(std::string(kind));
}

bool is_python_int(const Json& value) {
    return value.is_number_integer(); // a JSON bool is never a number here
}

// _coerce(value, default).
Json coerce(const Json& value, const Json& fallback) {
    if (fallback.is_boolean()) {
        return value.is_boolean() ? value : fallback;
    }
    if (is_python_int(fallback)) {
        return is_python_int(value) ? value : fallback;
    }
    if (fallback.is_array()) {
        if (!value.is_array()) {
            return fallback;
        }
        Json items = Json::array();
        for (const auto& item : value) {
            if (item.is_string() && items.size() < kMaxListItems) {
                items.push_back(item);
            }
        }
        return items;
    }
    if (fallback.is_string()) {
        return value.is_string() ? Json(py::strip(value.get_ref<const std::string&>())) : fallback;
    }
    return fallback;
}

// _params(kind, raw).
Json params(std::string_view kind, const Json& raw) {
    Json result = Json::object();
    for (const auto& [key, fallback] : kind_params(kind).items()) {
        const Json value = raw.is_object() && raw.contains(key) ? raw.at(key) : fallback;
        result[key] = coerce(value, fallback);
    }
    return result;
}

// _hotkey: "" for anything but a non-blank string, or one that cannot be read.
std::string hotkey(const Json& value) {
    if (!value.is_string() || py::strip(value.get_ref<const std::string&>()).empty()) {
        return "";
    }
    try {
        return hotkeys::normalize_hotkey(value.get_ref<const std::string&>());
    } catch (const std::exception&) {
        return "";
    }
}

// _spoken: whitespace runs collapsed, cut to 40 characters, blanks and case-folded repeats
// dropped, at most 8.
std::vector<std::string> spoken(const Json& value) {
    std::vector<std::string> names;
    if (!value.is_array()) {
        return names;
    }
    std::vector<std::string> folded;
    for (const auto& item : value) {
        if (!item.is_string()) {
            continue;
        }
        std::string joined;
        for (const auto& word : py::split(item.get_ref<const std::string&>())) {
            joined += (joined.empty() ? "" : " ") + word;
        }
        std::string name = py::slice(joined, 0, static_cast<std::int64_t>(kSpokenNameMaxChars));
        if (name.empty()) {
            continue;
        }
        std::string key = py::casefold(name);
        if (std::ranges::find(folded, key) != folded.end()) {
            continue;
        }
        folded.push_back(std::move(key));
        names.push_back(std::move(name));
    }
    if (names.size() > kMaxSpokenNames) {
        names.resize(kMaxSpokenNames);
    }
    return names;
}

const Json* field(const Json& item, std::string_view key) {
    const auto it = item.find(std::string(key));
    return it == item.end() ? nullptr : &*it;
}

bool nonblank_string(const Json* value) {
    return value != nullptr && value->is_string() &&
           !py::strip(value->get_ref<const std::string&>()).empty();
}

// _parse(item).
std::optional<ActionRow> parse(const Json& item) {
    if (!item.is_object()) {
        return std::nullopt;
    }
    const Json* id = field(item, "id");
    const Json* name = field(item, "name");
    const Json* kind = field(item, "kind");
    if (!nonblank_string(id) || !nonblank_string(name) || !nonblank_string(kind)) {
        return std::nullopt;
    }
    ActionRow row;
    row.kind = py::strip(kind->get_ref<const std::string&>());
    if (std::ranges::find(kKinds, row.kind) == kKinds.end()) {
        return std::nullopt;
    }
    const Json* shaping = field(item, "shaping");
    row.shaping = nonblank_string(shaping) ? py::strip(shaping->get_ref<const std::string&>())
                                           : std::string(kShapingCleanup);
    const Json* description = field(item, "description");
    const Json* enabled = field(item, "enabled");
    static const Json kNull;
    const Json* raw_params = field(item, "params");
    const Json* raw_hotkey = field(item, "hotkey");
    const Json* raw_spoken = field(item, "spoken_names");
    row.id = py::strip(id->get_ref<const std::string&>());
    row.name = py::strip(name->get_ref<const std::string&>());
    row.params = params(row.kind, raw_params != nullptr ? *raw_params : kNull);
    row.hotkey = hotkey(raw_hotkey != nullptr ? *raw_hotkey : kNull);
    row.spoken_names = spoken(raw_spoken != nullptr ? *raw_spoken : kNull);
    row.description = description != nullptr && description->is_string()
                          ? py::slice(py::strip(description->get_ref<const std::string&>()), 0,
                                      static_cast<std::int64_t>(kDescriptionMaxChars))
                          : std::string();
    row.enabled = enabled != nullptr && enabled->is_boolean() ? enabled->get<bool>() : true;
    return row;
}

} // namespace

Json ActionRow::to_json() const {
    Json spoken = Json::array();
    for (const auto& spoken_name : spoken_names) {
        spoken.push_back(spoken_name);
    }
    Json row = Json::object();
    row["id"] = id;
    row["name"] = name;
    row["kind"] = kind;
    row["params"] = params;
    row["hotkey"] = hotkey;
    row["shaping"] = shaping;
    row["spoken_names"] = std::move(spoken);
    row["description"] = description;
    row["enabled"] = enabled;
    return row;
}

std::vector<ActionRow> load_action_rows(const Json& actions) {
    Json settings = Json::object();
    settings[std::string(keys::actions.key())] = actions;
    const Json raw = keys::actions.read(settings);
    std::vector<ActionRow> rows;
    std::set<std::string> seen;
    if (!raw.is_array()) {
        return rows;
    }
    for (const auto& item : raw) {
        std::optional<ActionRow> row = parse(item);
        if (!row || seen.contains(row->id)) {
            continue;
        }
        seen.insert(row->id);
        rows.push_back(std::move(*row));
        if (rows.size() >= kMaxActions) {
            break;
        }
    }
    return rows;
}

} // namespace fv::backup::detail
