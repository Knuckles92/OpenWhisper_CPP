#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agents/agent_actions.h"
#include "agents/installed_agents.h"
#include "base/cancel.h"
#include "base/json.h"

// The parts of an ask (agents.py "ask" and "messages"): the command lines, the headless run, and
// reading what the agents print.
namespace fv::agents::detail {

// _find: the agent from the last scan while its program exists, else a fresh lookup (blocking).
std::optional<InstalledAgent> find_installed(std::string_view agent_id);

// How long a --help or features probe may take, in seconds.
inline constexpr double HELP_TIMEOUT_S = 15.0;
// stdout kept from a headless run, in bytes; whole lines are kept until it is reached.
inline constexpr std::size_t STDOUT_MAX_BYTES = 4 * 1024 * 1024;
// stderr lines kept from a headless run, and the characters kept of each.
inline constexpr std::size_t STDERR_LINES = 20;
inline constexpr std::int64_t STDERR_LINE_MAX = 400;
// How often a headless run looks at its cancel and its deadline.
inline constexpr std::chrono::milliseconds EXIT_POLL{100};

// Codex features that give the model a tool; each one present is switched off.
const std::vector<std::string_view>& codex_tool_feature_names();

// An agent model name an ask accepts: [\w.:/@\[\]+-]{1,200}.
bool valid_model(std::string_view model);

// The installed agent lacks a flag an ask needs; what() says what to do.
class TooOld : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The output of a read-only probe such as `--help`, stdout then stderr, cached per (path,
// version, args) when not empty; "" when it couldn't run.
std::string help(InstalledAgents& agents, const InstalledAgent& agent,
                 const std::vector<std::string>& args);
// Forgets the cached probe outputs (tests).
void forget_help();

struct CleanupPrompt {
    std::string_view system_prompt;
    std::string_view effort;
};

// `claude -p` with no tools, no MCP servers, hooks or saved session; writes mcp.json and
// settings.json into `workdir`. Throws TooOld.
std::vector<std::string> claude_ask_command(InstalledAgents& agents, const InstalledAgent& agent,
                                            std::string_view model,
                                            const std::filesystem::path& workdir,
                                            const CleanupPrompt* cleanup = nullptr);
// `codex exec` without the user's config, with every tool feature off; writes instructions.md
// into `workdir`. Throws TooOld.
std::vector<std::string> codex_ask_command(InstalledAgents& agents, const InstalledAgent& agent,
                                           std::string_view model,
                                           const std::filesystem::path& workdir,
                                           const CleanupPrompt* cleanup = nullptr);
// The tool features this Codex knows (an unknown name would stop the run); ["shell_tool"]
// without a readable list.
std::vector<std::string> codex_tool_features(InstalledAgents& agents, const InstalledAgent& agent);

// How a headless run ended.
struct HeadlessRun {
    std::string status; // done, timeout, canceled, unstartable
    std::optional<int> returncode;
    std::string stdout_text;
    std::string stderr_text; // the last STDERR_LINES non-empty lines, "\n" between
};

// Runs argv in the agent workspace with the agent environment and `prompt` on stdin as UTF-8
// bytes, until it exits, `cancel` fires or `deadline` passes; then kills its whole tree, which
// also ends helpers that outlive the agent while holding its pipes.
HeadlessRun run_headless(InstalledAgents& agents, const std::vector<std::string>& argv,
                         std::string_view prompt, std::chrono::steady_clock::time_point deadline,
                         const CancelToken& cancel, const std::filesystem::path& cwd = {});

// The "result" message `claude -p --output-format json` printed: that one object, or the last of
// a JSON array of the turn's messages (verbose output). nullopt when there is none.
std::optional<Json> claude_result(std::string_view stdout_text);
AskResult claude_answer(const HeadlessRun& run);
// The JSON objects `codex exec --json` printed, one per line.
std::vector<Json> codex_events(std::string_view stdout_text);
AskResult codex_answer(const HeadlessRun& run);
// Codex wraps provider errors as JSON in the message; the inner text reads better.
std::string codex_message(std::string_view message);

// The answer an ACP prompt turn streams, gathered from its notifications.
// Affinity: thread-safe (the connection's reader thread writes, the asking thread reads).
class AcpAnswer {
public:
    void on_notification(const std::string& method, const Json& params);
    std::string text() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::string> chunks_;
};

// The OpenCode child environment: both primary agents get the ask prompt and no tools.
std::map<std::string, std::string> opencode_env(const InstalledAgents& agents);
// The JSON overlay that environment carries in OPENCODE_CONFIG_CONTENT.
std::string opencode_overlay();
// The reply to an agent's request during an ask: every permission is rejected; any other request
// throws AcpError (-32601).
Json refuse_acp_request(const std::string& method, const Json& params);
// The model to ask with: "" keeps the session's own, nullopt means `model` isn't offered.
// `configured` is the user's OpenCode default.
std::optional<std::string> opencode_model(const Json& session, std::string_view model,
                                          std::string_view configured);
// How a prompt turn ended, given its result and the answer its updates streamed.
AskResult opencode_result(const Json& result, std::string_view answer);

AskResult answered(std::string_view agent_id, std::string_view answer);
std::string signed_out(std::string_view agent_id);
// Friendly copy for an agent's error; signed-out errors get the fix.
std::string failure(std::string_view agent_id, std::string_view error);
std::string stopped(std::string_view agent_id, const HeadlessRun& run);

} // namespace fv::agents::detail
