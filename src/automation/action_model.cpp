#include "automation/action_model.h"

#include <algorithm>
#include <exception>
#include <format>
#include <map>
#include <set>
#include <utility>

#include "automation/detail/pypath.h"
#include "automation/detail/pyvalue.h"
#include "automation/hooks_model.h"
#include "base/identity.h"
#include "base/ids.h"
#include "hotkeys/conflicts.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "pyport/unicode.h"
#include "settings/keys.h"
#include "text/cleanup_profiles.h"
#include "text/transforms.h"

namespace fv::automation {

namespace {

constexpr std::string_view kProfilePrefix = "profile:";
constexpr std::string_view kTransformPrefix = "transform:";
constexpr std::int64_t kProgramTimeoutMaxS = 300;
// _coerce keeps at most this many strings of a list parameter.
constexpr std::size_t kMaxListItems = 32;

template <std::size_t N>
bool one_of(const std::array<std::string_view, N>& values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

template <std::size_t N>
bool one_of(const std::array<std::string_view, N>& values, const Json& value) {
    return value.is_string() &&
           one_of(values, std::string_view(value.get_ref<const std::string&>()));
}

// _coerce(value, default): the value when it has the default's type (a bool is no int), else
// the default; strings are stripped, lists keep their first 32 strings.
Json coerce(const Json& value, const Json& fallback) {
    if (fallback.is_boolean()) {
        return value.is_boolean() ? value : fallback;
    }
    if (fallback.is_number_integer()) {
        return value.is_number_integer() ? value : fallback;
    }
    if (fallback.is_array()) {
        if (!value.is_array()) {
            return fallback;
        }
        Json items = Json::array();
        for (const Json& item : value) {
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

// _params(kind, raw): exactly the kind's parameters, coerced.
Json params_of(std::string_view kind, const Json& raw) {
    Json result = Json::object();
    for (const auto& [key, fallback] : PARAMS(kind).items()) {
        const Json& value = raw.is_object() && raw.contains(key) ? raw.at(key) : fallback;
        result[key] = coerce(value, fallback);
    }
    return result;
}

// _hotkey: "" for anything but a non-blank string, or one that cannot be read.
std::string hotkey_of(const Json& value, hotkeys::Vocabulary vocabulary) {
    if (!value.is_string() || py::strip(value.get_ref<const std::string&>()).empty()) {
        return "";
    }
    try {
        return hotkeys::normalize_hotkey(value.get_ref<const std::string&>(), vocabulary);
    } catch (const std::exception&) {
        return "";
    }
}

// _spoken: whitespace runs collapsed, cut to 40 code points, blanks and case-folded repeats
// dropped, at most 8.
std::vector<std::string> spoken_of(const std::vector<std::string>& values) {
    std::vector<std::string> names;
    std::vector<std::string> folded;
    for (const std::string& item : values) {
        std::string joined;
        for (const auto& word : py::split(item)) {
            joined += (joined.empty() ? "" : " ") + word;
        }
        std::string name = py::slice(joined, 0, static_cast<std::int64_t>(SPOKEN_NAME_MAX_CHARS));
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
    if (names.size() > MAX_SPOKEN_NAMES) {
        names.resize(MAX_SPOKEN_NAMES);
    }
    return names;
}

std::vector<std::string> string_items(const Json& value) {
    std::vector<std::string> items;
    if (value.is_array()) {
        for (const Json& item : value) {
            if (item.is_string()) {
                items.push_back(item.get<std::string>());
            }
        }
    }
    return items;
}

bool nonblank_string(const Json& value) {
    return value.is_string() && !py::strip(value.get_ref<const std::string&>()).empty();
}

// _parse(item).
std::optional<Action> parse(const Json& item, hotkeys::Vocabulary vocabulary) {
    if (!item.is_object()) {
        return std::nullopt;
    }
    const Json& id = detail::get(item, "id");
    const Json& name = detail::get(item, "name");
    const Json& kind = detail::get(item, "kind");
    if (!nonblank_string(id) || !nonblank_string(name) || !nonblank_string(kind)) {
        return std::nullopt;
    }
    Action action;
    action.kind = py::strip(kind.get_ref<const std::string&>());
    if (!one_of(ActionKind::ALL, std::string_view(action.kind))) {
        return std::nullopt;
    }
    const Json& shaping = detail::get(item, "shaping");
    action.shaping = nonblank_string(shaping) ? py::strip(shaping.get_ref<const std::string&>())
                                              : std::string(SHAPING_CLEANUP);
    const Json& description = detail::get(item, "description");
    const Json& enabled = detail::get(item, "enabled");
    action.id = py::strip(id.get_ref<const std::string&>());
    action.name = py::strip(name.get_ref<const std::string&>());
    action.params = params_of(action.kind, detail::get(item, "params"));
    action.hotkey = hotkey_of(detail::get(item, "hotkey"), vocabulary);
    action.spoken_names = spoken_of(string_items(detail::get(item, "spoken_names")));
    action.description = description.is_string()
                             ? py::slice(py::strip(description.get_ref<const std::string&>()), 0,
                                         static_cast<std::int64_t>(DESCRIPTION_MAX_CHARS))
                             : std::string();
    action.enabled = enabled.is_boolean() ? enabled.get<bool>() : true;
    return action;
}

// quote(text, safe=""): ASCII letters, digits and "_.-~" stay; every other byte of the UTF-8
// text is %XX in upper case.
std::string quote(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        const bool safe = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                          (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' ||
                          byte == '-' || byte == '~';
        if (safe) {
            out += c;
        } else {
            out += '%';
            out += kHex[byte >> 4];
            out += kHex[byte & 0x0f];
        }
    }
    return out;
}

std::string replace_all(std::string_view text, std::string_view from, std::string_view to) {
    std::string out;
    std::size_t start = 0;
    while (true) {
        const auto at = text.find(from, start);
        if (at == std::string_view::npos) {
            out += text.substr(start);
            return out;
        }
        out += text.substr(start, at - start);
        out += to;
        start = at + from.size();
    }
}

// The `or ""` of a parameter read as text: a string, "" for anything falsy.
std::string text_param(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (!detail::truthy(value)) {
        return {};
    }
    // os.path and len() refuse a non-text value; save_action only ever passes text.
    const char* type = value.is_boolean()          ? "bool"
                       : value.is_number_integer() ? "int"
                       : value.is_number_float()   ? "float"
                       : value.is_array()          ? "list"
                                                   : "dict";
    throw py::TypeError(std::format("expected str, bytes or os.PathLike object, not {}", type));
}

// int(value), with Python's exception types and messages.
std::int64_t int_param(const Json& value) {
    const auto result = py::py_int_checked(value);
    if (result) {
        return *result;
    }
    switch (result.error()) {
    case py::IntError::Type:
        throw py::TypeError(std::format(
            "int() argument must be a string, a bytes-like object or a real number, not '{}'",
            value.is_null()    ? "NoneType"
            : value.is_array() ? "list"
                               : "dict"));
    case py::IntError::Overflow:
        throw py::OverflowError("cannot convert float infinity to integer");
    default:
        if (value.is_string()) {
            throw py::ValueError(std::format("invalid literal for int() with base 10: {}",
                                             py::repr(value.get_ref<const std::string&>())));
        }
        throw py::ValueError("cannot convert float NaN to integer");
    }
}

void validate_shaping(std::string_view shaping, const Json& settings) {
    if (shaping == SHAPING_RAW || shaping == SHAPING_CLEANUP) {
        return;
    }
    if (shaping.starts_with(kProfilePrefix)) {
        if (!text::find_cleanup_profile(settings, shaping.substr(kProfilePrefix.size()))) {
            throw py::ValueError("That cleanup profile no longer exists. Choose another.");
        }
        return;
    }
    if (shaping.starts_with(kTransformPrefix)) {
        if (!text::find_transform(settings, shaping.substr(kTransformPrefix.size()))) {
            throw py::ValueError("That transform no longer exists. Choose another.");
        }
        return;
    }
    throw py::ValueError("Choose how the words are prepared.");
}

void validate_link_params(const Action& action) {
    const std::string template_text = text_param(action.param("template"));
    if (template_text.empty()) {
        throw py::ValueError("Enter the link to open.");
    }
    if (py::len(template_text) > TEMPLATE_MAX_CHARS) {
        throw py::ValueError(
            std::format("Use a link of {} characters or fewer.", TEMPLATE_MAX_CHARS));
    }
    expand_link(template_text, "");
}

void validate_agent_params(const Action& action) {
    if (!one_of(AGENTS, action.param("agent"))) {
        throw py::ValueError("Choose Claude Code, Codex or OpenCode.");
    }
    if (!one_of(AgentMode::ALL, action.param("mode"))) {
        throw py::ValueError("Choose whether to open a session or ask for an answer.");
    }
    if (!one_of(AgentSurface::ALL, action.param("surface"))) {
        throw py::ValueError("Choose where the session opens.");
    }
    const Json folder = action.param("folder");
    if (!detail::truthy(action.param("new_folder")) && detail::truthy(folder) &&
        !detail::isabs(text_param(folder))) {
        throw py::ValueError("Choose the working folder with its full path.");
    }
    if (!one_of(Output::ALL, action.param("output"))) {
        throw py::ValueError("Choose where the answer goes.");
    }
}

void validate_webhook_params(const Action& action) {
    hooks::validate_url(text_param(action.param("url")));
    if (!one_of(Output::ALL, action.param("output"))) {
        throw py::ValueError("Choose where the reply goes.");
    }
}

void validate_program_params(const Action& action) {
    const std::string program = text_param(action.param("program"));
    if (program.empty()) {
        throw py::ValueError("Choose the program to run.");
    }
    if (!detail::isabs(program)) {
        throw py::ValueError("Choose the program with its full path.");
    }
    if (!one_of(Output::ALL, action.param("output"))) {
        throw py::ValueError("Choose where the program's output goes.");
    }
    const std::int64_t timeout = int_param(action.param("timeout_s"));
    if (timeout < 1 || timeout > kProgramTimeoutMaxS) {
        throw py::ValueError(
            std::format("Use a time limit from 1 to {} seconds.", kProgramTimeoutMaxS));
    }
}

void validate_params(const Action& action) {
    if (action.kind == ActionKind::OPEN_LINK) {
        validate_link_params(action);
    } else if (action.kind == ActionKind::AGENT) {
        validate_agent_params(action);
    } else if (action.kind == ActionKind::WEBHOOK) {
        validate_webhook_params(action);
    } else if (action.kind == ActionKind::PROGRAM) {
        validate_program_params(action);
    }
}

Json strings_json(const std::vector<std::string>& items) {
    Json list = Json::array();
    for (const auto& item : items) {
        list.push_back(item);
    }
    return list;
}

Json rows_json(const std::vector<Action>& actions) {
    Json rows = Json::array();
    for (const Action& action : actions) {
        rows.push_back(action.to_dict());
    }
    return rows;
}

std::string product_text(std::string_view format_with_product) {
    return replace_all(format_with_product, "{product}", identity().product);
}

} // namespace

const std::vector<std::pair<std::string_view, std::string_view>>& AGENT_NAMES() {
    static const std::vector<std::pair<std::string_view, std::string_view>> names = {
        {"claude_code", "Claude Code"}, {"codex", "Codex"}, {"opencode_cli", "OpenCode"}};
    return names;
}

std::optional<std::string_view> agent_display_name(std::string_view agent_id) {
    for (const auto& [id, name] : AGENT_NAMES()) {
        if (id == agent_id) {
            return name;
        }
    }
    return std::nullopt;
}

const Json& PARAMS(std::string_view kind) {
    static const Json table = [] {
        Json t = Json::object();
        t[std::string(ActionKind::OPEN_LINK)] = Json{{"template", ""}};
        t[std::string(ActionKind::AGENT)] = Json{{"agent", "claude_code"},
                                                 {"mode", std::string(AgentMode::HANDOFF)},
                                                 {"surface", std::string(AgentSurface::AUTO)},
                                                 {"folder", ""},
                                                 {"new_folder", false},
                                                 {"include_selection", false},
                                                 {"model", ""},
                                                 {"output", std::string(Output::SCRATCHPAD)}};
        t[std::string(ActionKind::CHANNEL)] = Json::object();
        t[std::string(ActionKind::WEBHOOK)] =
            Json{{"url", ""}, {"output", std::string(Output::NONE)}};
        t[std::string(ActionKind::PROGRAM)] = Json{{"program", ""},
                                                   {"args", Json::array()},
                                                   {"output", std::string(Output::NONE)},
                                                   {"timeout_s", 30}};
        t[std::string(ActionKind::CLIPBOARD)] = Json::object();
        t[std::string(ActionKind::SCRIPTS)] = Json::object();
        return t;
    }();
    static const Json kNone = Json::object();
    const auto found = table.find(std::string(kind));
    return found == table.end() ? kNone : *found;
}

std::string new_id() {
    return random_hex(12);
}

std::string action_secret_name(std::string_view action_id) {
    return std::format("{}ACTION_SECRET_{}", identity().env_prefix, action_id);
}

Json Action::param(std::string_view key) const {
    const Json& stored = detail::get(params, key);
    if (params.is_object() && params.contains(std::string(key))) {
        return stored;
    }
    return detail::get(PARAMS(kind), key);
}

std::string Action::profile_id() const {
    return shaping.starts_with(kProfilePrefix) ? shaping.substr(kProfilePrefix.size())
                                               : std::string();
}

std::string Action::transform_id() const {
    return shaping.starts_with(kTransformPrefix) ? shaping.substr(kTransformPrefix.size())
                                                 : std::string();
}

Json Action::to_dict() const {
    Json data = Json::object();
    data["id"] = id;
    data["name"] = name;
    data["kind"] = kind;
    data["params"] = params.is_object() ? params : Json::object();
    data["hotkey"] = hotkey;
    data["shaping"] = shaping;
    data["spoken_names"] = strings_json(spoken_names);
    data["description"] = description;
    data["enabled"] = enabled;
    return data;
}

Json Action::public_dict() const {
    Json data = Json::object();
    data["id"] = id;
    data["name"] = name;
    data["kind"] = kind;
    data["description"] = description;
    data["spoken_names"] = strings_json(spoken_names);
    data["hotkey"] = hotkey;
    data["shaping"] = shaping;
    data["enabled"] = enabled;
    if (kind == ActionKind::AGENT) {
        data["agent"] = param("agent");
        data["mode"] = param("mode");
    }
    return data;
}

bool operator==(const Action& a, const Action& b) {
    return a.id == b.id && a.name == b.name && a.kind == b.kind &&
           py::py_equal(a.params, b.params) && a.hotkey == b.hotkey && a.shaping == b.shaping &&
           a.spoken_names == b.spoken_names && a.description == b.description &&
           a.enabled == b.enabled;
}

const std::vector<Preset>& PRESETS() {
    static const std::vector<Preset> presets = [] {
        std::vector<Preset> list;
        const auto add = [&](std::string id, std::string name, std::string_view kind, Json params,
                             std::string description, std::string icon, std::string note = {}) {
            list.push_back(Preset{std::move(id), std::move(name), std::string(kind),
                                  std::move(params), std::move(description), std::move(icon),
                                  std::string(SHAPING_CLEANUP), std::move(note)});
        };
        add("agent", "Agent hotkey", ActionKind::AGENT,
            Json{{"agent", "claude_code"}, {"mode", std::string(AgentMode::HANDOFF)}},
            "Start a session with your coding agent from what you say.", "wand-purple.svg");
        add("claude-chat", "Ask Claude", ActionKind::OPEN_LINK,
            Json{{"template", "claude://claude.ai/new?q={text}"}},
            "Open a new Claude Desktop chat with your words ready to send.", "world-blue.svg");
        add("chatgpt-app", "ChatGPT app", ActionKind::OPEN_LINK,
            Json{{"template", "codex://new?prompt={text}"}},
            "Open a new chat in the ChatGPT desktop app with your words ready to send.",
            "world-blue.svg");
        add("chatgpt-web", "ChatGPT on the web", ActionKind::OPEN_LINK,
            Json{{"template", "https://chatgpt.com/?q={text}"}},
            "Open chatgpt.com with your words in the message box.", "world-blue.svg",
            "ChatGPT doesn't document this link, so it may stop working.");
        add("quick-answer", "Quick answer", ActionKind::AGENT,
            Json{{"agent", "claude_code"},
                 {"mode", std::string(AgentMode::ASK)},
                 {"output", std::string(Output::SCRATCHPAD)}},
            "Ask your agent a question and read the answer in the Scratchpad.", "wand-purple.svg");
        add("claude-code-session", "Tell my Claude Code session", ActionKind::CHANNEL,
            Json::object(),
            product_text("Send your words into the Claude Code session running the {product} "
                         "channel."),
            "notes-blue.svg");
        add("webhook", "Webhook", ActionKind::WEBHOOK, Json{{"url", ""}},
            "Send your words to a web address, such as n8n or Home Assistant.", "server-blue.svg");
        add("program", "Run a program", ActionKind::PROGRAM, Json{{"program", ""}},
            "Run a program or script with your words on its input.", "cpu-blue.svg");
        add("copy", "Copy only", ActionKind::CLIPBOARD, Json::object(),
            "Copy your words without pasting them.", "copy-gray.svg");
        add("scripts", "Send to my scripts", ActionKind::SCRIPTS, Json::object(),
            product_text("Only tell programs listening to {product}'s events."), "bolt-green.svg");
        return list;
    }();
    return presets;
}

std::vector<Action> load_actions(const Json& settings, hotkeys::Vocabulary vocabulary) {
    static const Json kEmpty = Json::object();
    const Json raw = keys::actions.read(settings.is_object() ? settings : kEmpty);
    std::vector<Action> actions;
    std::set<std::string> seen;
    if (!raw.is_array()) {
        return actions;
    }
    for (const Json& item : raw) {
        std::optional<Action> action = parse(item, vocabulary);
        if (!action || seen.contains(action->id)) {
            continue;
        }
        seen.insert(action->id);
        actions.push_back(std::move(*action));
        if (actions.size() >= MAX_ACTIONS) {
            break;
        }
    }
    return actions;
}

std::optional<Action> find_action(const Json& settings, std::string_view action_id,
                                  hotkeys::Vocabulary vocabulary) {
    for (Action& action : load_actions(settings, vocabulary)) {
        if (action.id == action_id) {
            return std::move(action);
        }
    }
    return std::nullopt;
}

std::string link_scheme(std::string_view template_text) {
    const auto colon = template_text.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return "";
    }
    const std::u32string head = py::to_code_points(template_text.substr(0, colon));
    if (!py::is_alpha(head.front())) {
        return "";
    }
    for (const char32_t c : head) {
        if (!py::is_alnum(c) && c != U'+' && c != U'-' && c != U'.') {
            return "";
        }
    }
    return py::lower(template_text.substr(0, colon));
}

std::string expand_link(std::string_view template_text, std::string_view text) {
    const std::string scheme = link_scheme(template_text);
    if (scheme.empty()) {
        throw py::ValueError("Start the link with a scheme, such as https:// or claude://.");
    }
    if (one_of(REFUSED_SCHEMES, std::string_view(scheme))) {
        throw py::ValueError(std::format("Links that start with {}: can't be used.", scheme));
    }
    return replace_all(template_text, "{text}", quote(text));
}

std::string action_hotkey_conflict(std::string_view hotkey, const Json& settings,
                                   std::string_view exclude_id) {
    text::ShortcutContext context;
    for (const Action& action : load_actions(settings, context.vocabulary)) {
        context.actions.push_back({action.id, action.name, action.hotkey});
    }
    hotkeys::ConflictOptions options;
    options.exclude = {std::string(hotkeys::kAction), std::string(exclude_id)};
    options.bindings = text::hotkey_bindings(settings, context);
    options.vocabulary = context.vocabulary;
    options.platform = context.platform;
    return hotkeys::hotkey_conflict(hotkey, settings, options);
}

void validate_action(const Action& action, const Json& settings) {
    const std::string name = py::strip(action.name);
    if (name.empty()) {
        throw py::ValueError("Give the action a name.");
    }
    if (py::len(name) > NAME_MAX_CHARS) {
        throw py::ValueError(
            std::format("Use a name with {} characters or fewer.", NAME_MAX_CHARS));
    }
    if (!one_of(ActionKind::ALL, std::string_view(action.kind))) {
        throw py::ValueError("Choose what the action does.");
    }
    if (py::len(action.description) > DESCRIPTION_MAX_CHARS) {
        throw py::ValueError(
            std::format("Use a description of {} characters or fewer.", DESCRIPTION_MAX_CHARS));
    }
    const std::vector<Action> existing = load_actions(settings);
    std::vector<Action> others;
    for (const Action& other : existing) {
        if (other.id != action.id) {
            others.push_back(other);
        }
    }
    const std::string folded_name = py::casefold(name);
    for (const Action& other : others) {
        if (py::casefold(other.name) == folded_name) {
            throw py::ValueError("An action with that name already exists.");
        }
    }
    // A dict comprehension: of two actions answering to one name, the later one is named.
    std::map<std::string, std::string> taken;
    for (const Action& other : others) {
        taken[py::casefold(other.name)] = other.name;
        for (const std::string& spoken : other.spoken_names) {
            taken[py::casefold(spoken)] = other.name;
        }
    }
    std::vector<std::string> answers_to{name};
    answers_to.insert(answers_to.end(), action.spoken_names.begin(), action.spoken_names.end());
    for (const std::string& spoken : answers_to) {
        const auto owner = taken.find(py::casefold(spoken));
        if (owner != taken.end() && !owner->second.empty()) {
            throw py::ValueError(std::format("\"{}\" already names {}. Choose another name.",
                                             spoken, owner->second));
        }
    }
    if (others.size() == existing.size() && existing.size() >= MAX_ACTIONS) {
        throw py::ValueError(
            std::format("You can keep up to {} actions. Delete one to add another.", MAX_ACTIONS));
    }
    validate_shaping(action.shaping, settings);
    validate_params(action);
    const std::string conflict = action_hotkey_conflict(action.hotkey, settings, action.id);
    if (!conflict.empty()) {
        throw py::ValueError(
            std::format("That shortcut is already used by {}. Choose another.", conflict));
    }
}

void save_action(Json& settings, const Action& action) {
    Action clean;
    clean.id = py::strip(action.id);
    clean.name = py::strip(action.name);
    clean.kind = action.kind;
    clean.params = params_of(action.kind, action.params);
    clean.hotkey = hotkey_of(Json(action.hotkey), hotkeys::platform_vocabulary());
    clean.shaping =
        py::strip(action.shaping.empty() ? std::string_view(SHAPING_CLEANUP) : action.shaping);
    clean.spoken_names = spoken_of(action.spoken_names);
    clean.description = py::strip(action.description);
    clean.enabled = action.enabled;
    if (clean.id.empty()) {
        throw py::ValueError("This action has no id.");
    }
    validate_action(clean, settings);
    std::vector<Action> actions = load_actions(settings);
    const auto found =
        std::ranges::find_if(actions, [&](const Action& other) { return other.id == clean.id; });
    if (found == actions.end()) {
        actions.push_back(std::move(clean));
    } else {
        *found = std::move(clean);
    }
    settings[std::string(keys::actions.key())] = rows_json(actions);
}

void delete_action(Json& settings, std::string_view action_id) {
    std::vector<Action> kept;
    for (Action& action : load_actions(settings)) {
        if (action.id != action_id) {
            kept.push_back(std::move(action));
        }
    }
    settings[std::string(keys::actions.key())] = rows_json(kept);
}

dictation::RecordingAction to_recording_action(const Action& action) {
    dictation::RecordingAction recording;
    recording.name = action.name;
    recording.kind = action.kind;
    recording.params = action.params.is_object() ? action.params : Json::object();
    // The pipeline routes only to an enabled action and shapes its words as the action asks.
    recording.id = action.id;
    recording.shaping = action.shaping;
    recording.enabled = action.enabled;
    return recording;
}

} // namespace fv::automation
