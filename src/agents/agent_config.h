#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"

// What the installed agents' own configuration says (services/installed_agents.py, "the agents'
// own configuration"): the default model each one is set to, and Codex's cached model catalog.
// Nothing here starts a process.
namespace fv::agents {

// Agent ids: settings' MeetingAgentCore values.
inline constexpr std::string_view CLAUDE_CODE = "claude_code";
inline constexpr std::string_view CODEX = "codex";
inline constexpr std::string_view OPENCODE = "opencode_cli";

// One choice in an agent's model list; value "" is the agent's own default.
struct AgentModel {
    std::string value;
    std::string label;
    bool operator==(const AgentModel&) const = default;
};

// {slug: reasoning efforts} in the catalog's order.
using ModelEfforts = std::vector<std::pair<std::string, std::vector<std::string>>>;

// Drops // and /* */ comments outside strings, then every comma before a closing bracket or
// brace, inside strings too, as _strip_jsonc's final re.sub does.
std::string strip_jsonc(std::string_view text);

// The top-level `model` of a Codex config.toml, as tomllib.load(...).get("model") reads it:
// nullopt when there is none, when it is not a string, or when the text before the first table
// header is not TOML this reader understands (tomllib would refuse it). Basic, literal and
// multi-line strings, comments and dotted or quoted keys are read; tables are not.
std::optional<std::string> codex_config_model(std::string_view toml);

// _codex_models over a parsed models_cache.json: listed entries (visibility absent, null or
// "list") with a slug, labelled by display_name.
std::vector<AgentModel> codex_models_from(const Json& cache);
// codex_model_efforts over a parsed models_cache.json.
ModelEfforts codex_model_efforts_from(const Json& cache);

// Reads the configuration files the agents keep under the user's home. Environment variables
// (CLAUDE_CONFIG_DIR, CODEX_HOME, XDG_CONFIG_HOME) come from `env`, so tests and the app's
// InstalledAgents decide which environment counts. A relative folder (an empty home) reads as
// no file.
// Affinity: thread-safe (immutable; every call reads the files again).
class AgentConfig {
public:
    using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;

    AgentConfig(EnvLookup env, std::filesystem::path home);
    // This process's environment and home folder.
    static AgentConfig system();

    const std::filesystem::path& home() const { return home_; }
    std::optional<std::string> env(std::string_view name) const;

    std::filesystem::path claude_config_dir() const;   // $CLAUDE_CONFIG_DIR or ~/.claude
    std::filesystem::path codex_home() const;          // $CODEX_HOME or ~/.codex
    std::filesystem::path opencode_config_dir() const; // ($XDG_CONFIG_HOME or ~/.config)/opencode

    // The model the user set as the agent's default, stripped, or "" when unset.
    std::string configured_default_model(std::string_view agent_id) const;
    std::vector<AgentModel> codex_models() const;
    ModelEfforts codex_model_efforts() const;
    // Whether omitting user config for an isolated headless cleanup preserves its route.
    bool supports_isolated_cleanup(std::string_view agent_id) const;
    // "{Name} default ({configured})", or "{Name} default".
    std::string default_model_label(std::string_view agent_id) const;

private:
    EnvLookup env_;
    std::filesystem::path home_;
};

// _read_json: a UTF-8 file holding a JSON object (after strip_jsonc when `jsonc`), else nullopt.
std::optional<Json> read_json_object(const std::filesystem::path& file, bool jsonc = false);

} // namespace fv::agents
