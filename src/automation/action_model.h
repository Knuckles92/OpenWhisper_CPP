#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "dictation/messages.h"
#include "hotkeys/grammar.h"

// Saved actions: a name, what to do with the words, and how they are shaped first
// (services/actions/model.py). Only Settings creates or changes actions; the live API and MCP
// list and run them. Parameters are kept per kind and unknown keys are dropped on load, so an
// old or hand-edited settings file cannot smuggle in fields a kind never reads. A webhook
// action's signing secret is in the credential store (automation/secrets.h), never here.
// Validation errors are py::ValueError with the sentence shown to the user; the mutators edit
// the settings mapping they are given, for SettingsStore::mutate, and change nothing when they
// throw. Every function is thread-safe and pure over its arguments.
namespace fv::automation {

inline constexpr std::size_t MAX_ACTIONS = 50;
// Lengths in code points.
inline constexpr std::size_t NAME_MAX_CHARS = 80;
inline constexpr std::size_t DESCRIPTION_MAX_CHARS = 300;
inline constexpr std::size_t MAX_SPOKEN_NAMES = 8;
inline constexpr std::size_t SPOKEN_NAME_MAX_CHARS = 40;
inline constexpr std::size_t TEMPLATE_MAX_CHARS = 2000;
inline constexpr std::string_view AGENT_ACTION_ID = "agent";

struct ActionKind {
    static constexpr std::string_view OPEN_LINK = "open_link";
    static constexpr std::string_view AGENT = "agent";
    static constexpr std::string_view CHANNEL = "channel";
    static constexpr std::string_view WEBHOOK = "webhook";
    static constexpr std::string_view PROGRAM = "program";
    static constexpr std::string_view CLIPBOARD = "clipboard";
    static constexpr std::string_view SCRIPTS = "scripts";
    static constexpr std::array<std::string_view, 7> ALL = {OPEN_LINK, AGENT,     CHANNEL, WEBHOOK,
                                                            PROGRAM,   CLIPBOARD, SCRIPTS};
};

// Where a webhook's, program's or agent's answer goes.
struct Output {
    static constexpr std::string_view NONE = "none";
    static constexpr std::string_view PASTE = "paste";
    static constexpr std::string_view SCRATCHPAD = "scratchpad";
    static constexpr std::string_view NOTICE = "notice";
    static constexpr std::string_view CLIPBOARD = "clipboard";
    static constexpr std::array<std::string_view, 5> ALL = {NONE, PASTE, SCRATCHPAD, NOTICE,
                                                            CLIPBOARD};
};

struct AgentMode {
    static constexpr std::string_view HANDOFF = "handoff";
    static constexpr std::string_view ASK = "ask";
    static constexpr std::array<std::string_view, 2> ALL = {HANDOFF, ASK};
};

struct AgentSurface {
    static constexpr std::string_view AUTO = "auto";
    static constexpr std::string_view APP = "app";
    static constexpr std::string_view TERMINAL = "terminal";
    static constexpr std::array<std::string_view, 3> ALL = {AUTO, APP, TERMINAL};
};

// Installed agents an agent action can use (the installed-agent ids), and their names.
inline constexpr std::array<std::string_view, 3> AGENTS = {"claude_code", "codex", "opencode_cli"};
// AGENT_NAMES: (id, name) in AGENTS order: "Claude Code", "Codex", "OpenCode".
const std::vector<std::pair<std::string_view, std::string_view>>& AGENT_NAMES();
// AGENT_NAMES[agent], or nullopt for an id that is not in AGENTS.
std::optional<std::string_view> agent_display_name(std::string_view agent_id);

inline constexpr std::string_view SHAPING_RAW = "raw";
inline constexpr std::string_view SHAPING_CLEANUP = "cleanup";

// Link schemes an open_link action may never use.
inline constexpr std::array<std::string_view, 6> REFUSED_SCHEMES = {
    "file", "javascript", "data", "vbscript", "about", "blob"};

// PARAMS[kind]: each parameter with its default, in order; the default's type is the type the
// parameter accepts. An empty object for a kind without parameters or one that is not a kind.
const Json& PARAMS(std::string_view kind);

// secrets.token_hex(6).
std::string new_id();
// The credential name of a webhook action's signing secret: "<env_prefix>ACTION_SECRET_<id>".
std::string action_secret_name(std::string_view action_id);

// Affinity: value type.
struct Action {
    std::string id;
    std::string name;
    std::string kind;
    Json params = Json::object();
    std::string hotkey;
    std::string shaping = std::string(SHAPING_CLEANUP);
    std::vector<std::string> spoken_names;
    std::string description;
    bool enabled = true;

    // The stored parameter, else the kind's default, else null.
    Json param(std::string_view key) const;
    // The id after "profile:" or "transform:" in shaping, else "".
    std::string profile_id() const;
    std::string transform_id() const;
    // {id, name, kind, params, hotkey, shaping, spoken_names, description, enabled}.
    Json to_dict() const;
    // What the API and MCP may see: no URLs, paths, folders or arguments; an agent action adds
    // its agent and mode.
    Json public_dict() const;

    // The dataclass's ==: params compare as Python dicts do (key order ignored).
    friend bool operator==(const Action& a, const Action& b);
};

// One tile of the Add action gallery.
struct Preset {
    std::string id;
    std::string name;
    std::string kind;
    Json params;
    std::string description;
    std::string icon;
    std::string shaping = std::string(SHAPING_CLEANUP);
    std::string note;
};

// PRESETS, in gallery order; product names in descriptions come from identity().
const std::vector<Preset>& PRESETS();

// Saved actions; malformed rows, unknown kinds, repeated ids (after stripping) and rows past the
// 50th are skipped. Text is stripped, parameters coerced to their defaults' types, spoken names
// tidied, shortcuts normalized (an unreadable one reads as none).
std::vector<Action> load_actions(const Json& settings,
                                 hotkeys::Vocabulary vocabulary = hotkeys::platform_vocabulary());
std::optional<Action> find_action(const Json& settings, std::string_view action_id,
                                  hotkeys::Vocabulary vocabulary = hotkeys::platform_vocabulary());

// The lower-cased scheme of a link template ("claude" for "claude://..."), or "" when it has
// none: the text before the first ":" must start with a letter and hold only letters, digits
// and "+-.".
std::string link_scheme(std::string_view template_text);
// `template_text` with "{text}" replaced by the percent-encoded words (quote(text, safe="")).
// Throws py::ValueError when the template has no scheme or uses a refused one.
std::string expand_link(std::string_view template_text, std::string_view text);

// The standard shortcut, profile, transform or other action already using `hotkey`, or "".
std::string action_hotkey_conflict(std::string_view hotkey, const Json& settings,
                                   std::string_view exclude_id = {});

// Throws py::ValueError with the user-facing reason `action` cannot be saved: its name, kind,
// description, a name or spoken name another action already answers to (case-folded), the
// 50-action limit (for a new id only), its shaping, its kind's parameters, its shortcut.
void validate_action(const Action& action, const Json& settings);
// Adds `action` (cleaned as load_actions cleans) at the end, or replaces the one with its id in
// place; throws "This action has no id." or as validate_action.
void save_action(Json& settings, const Action& action);
// Removes an action; the saved list is rewritten from load_actions.
void delete_action(Json& settings, std::string_view action_id);

// The pipeline's view of an action (dictation::RecordingAction): its name, kind and parameters.
dictation::RecordingAction to_recording_action(const Action& action);

} // namespace fv::automation
