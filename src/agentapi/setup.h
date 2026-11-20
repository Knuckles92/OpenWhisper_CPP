#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "base/paths.h"

// Copyable connection instructions for the in-app MCP server (services/agent_mcp/setup.py); no
// credential is written to disk. Each format takes the access token so a copy is ready to paste,
// or TOKEN_PLACEHOLDER for the user to replace. The MCP server's name in these snippets is the
// product's, lower case ("fiorivoce"), as is the Claude Code channel's.
// Affinity: functions; any thread.
namespace fv::agentapi {

inline constexpr std::string_view TOKEN_PLACEHOLDER = "<PASTE_TOKEN>";

// The server name of the MCP configuration and of the channel's .mcp.json entry and launch
// command (CHANNEL_SERVER_NAME): identity().product in lower case.
std::string server_name();

// {"mcpServers": {<name>: {"url", "headers": {"Authorization": "Bearer <token>"}}}}, indented by
// two, non-ASCII kept.
std::string client_config(std::string_view url, std::string_view token = TOKEN_PLACEHOLDER);
// claude mcp add --transport http --scope user <name> <url> --header "Authorization: Bearer ..."
std::string claude_command(std::string_view url, std::string_view token = TOKEN_PLACEHOLDER);
// Codex's config.toml table [mcp_servers.<name>] with url and http_headers.
std::string chatgpt_config(std::string_view url, std::string_view token = TOKEN_PLACEHOLDER);

// How Claude Code starts the channel: this executable, by absolute path, with --channel.
std::vector<std::string> channel_command_line(const std::filesystem::path& executable);
// _channel_env: {"<env_prefix>DATA_DIR": data_root} when the data root is not the per-user
// folder (compared as os.path.normcase(os.path.abspath(...))), else {}.
Json channel_env(const std::filesystem::path& data_root,
                 const std::filesystem::path& local_app_dir);
// The .mcp.json entry that lets Claude Code start the channel; it holds no token (the channel
// reads it from the credential store).
std::string claude_channel_config(const std::filesystem::path& executable,
                                  const std::filesystem::path& data_root,
                                  const std::filesystem::path& local_app_dir);
std::string claude_channel_config(const std::filesystem::path& executable, const Paths& paths);
// claude --dangerously-load-development-channels server:<name>
std::string claude_channel_command();

// What to tell an agent to connect. `host_name` names the computer running the app when the
// URL is its localhost (this computer's name when nullopt or empty); without `token` the prompt
// has the agent ask the user for it.
std::string agent_prompt(std::string_view url, std::optional<std::string_view> host_name = {},
                         std::string_view token = "");

} // namespace fv::agentapi
