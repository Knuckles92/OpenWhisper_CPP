#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agents/installed_agents.h"
#include "agents/loopback_mcp.h"
#include "base/cancel.h"
#include "base/json.h"
#include "process/launcher.h"

// Running one pass through an installed agent (meeting/agent/installed/drivers.py). A driver
// turns a PassRequest (instructions, prompt, tools, limits) into a finished run of the user's
// agent and a PassOutcome. The tools are always the app's MCP tools: the headless drivers (Claude
// Code, Codex) point a fresh CLI process at a pass-scoped endpoint, and the OpenCode driver
// routes calls from its long-lived ACP process to the pass by session id.
//
// Lockdown, per agent (as verified for OpenWhisper against Claude Code 2.1, Codex 0.158 and
// OpenCode 2.0):
//   Claude Code: `--tools ""` removes every built-in tool, --strict-mcp-config drops the user's
//     MCP servers, --allowedTools pre-approves ours. A pass with a read_folder gets Read too,
//     fenced to that folder.
//   Codex: the shell and every optional tool feature disabled, read-only sandbox, our server
//     pre-approved, AGENTS.md and notify hooks off.
//   OpenCode: its build and plan agents are overridden for this process only, with deny-all
//     permissions except <server>_*.
namespace fv::agents {

// The agent cannot run passes (missing, too old, signed out); what() is the sentence to show.
class AgentUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Activity kinds a driver reports while a pass runs.
inline constexpr std::array<std::string_view, 7> EVENT_KINDS{"start", "thinking", "writing", "tool",
                                                             "turn",  "retry",    "settled"};

// Steady-clock time of a pass's latest sign of life.
// Affinity: thread-safe.
class Heartbeat {
public:
    Heartbeat();
    void touch();
    std::chrono::steady_clock::time_point last() const;
    // True the first time it is called: the event callback warns once per pass, then stays quiet.
    bool first_callback_failure();

private:
    std::atomic<std::chrono::steady_clock::rep> last_;
    std::atomic<bool> callback_failed_{false};
};

// One pass for an agent to run. Copies share the heartbeat.
struct PassRequest {
    std::string system_prompt;  // the pass's charter
    std::string user_prompt;    // state snapshot and transcript, or the task's input
    Json tools = Json::array(); // MCP tool definitions the agent may call
    ToolHandler handler;        // answers those calls
    std::string model;          // "" keeps the agent's own default
    std::string effort;         // reasoning effort (low, medium, high), or ""
    double timeout_s = 120.0;   // hard wall for the whole run, in seconds
    // Fail after this many seconds without any output or tool call; nullopt never stalls.
    std::optional<double> stall_s = 75.0;
    // cb(kind, tool) with a kind from EVENT_KINDS, on whichever thread saw the event.
    std::function<void(std::string_view kind, std::string_view tool)> on_event;
    CancelToken cancel;
    // Which of the driver's start-time personas system_prompt is (build or plan); a driver that
    // fixes its system prompt per process uses it instead of system_prompt.
    std::string persona = "build";
    // A folder the agent may read files in with its own Read tool, and nothing outside it; empty
    // for none. Only Claude Code takes one, in its long form (an 8.3 short path is refused).
    std::filesystem::path read_folder;
    // PNG files sent along with the prompt, in order (Codex, OpenCode).
    std::vector<std::filesystem::path> images;
    // Touched on every sign of life.
    std::shared_ptr<Heartbeat> heartbeat = std::make_shared<Heartbeat>();

    // Touches the heartbeat and reports the event; a callback that throws is logged.
    void emit(std::string_view kind, std::string_view tool = "") const;
    // `handler` that also counts as progress and reports the tool.
    ToolHandler tracked_handler() const;
};

// How a pass ended. `text` is the agent's closing message; `usage` holds prompt_tokens,
// completion_tokens, total_tokens, requests and, when the agent reports them, cost_usd or
// cached_tokens.
struct PassOutcome {
    bool ok = false;
    std::string text;
    std::string error;
    Json usage = Json::object();
    bool canceled = false;
};

// How a meeting pass ends: its work is the tool calls, not the reply.
inline constexpr std::string_view PASS_CLOSING =
    "When the work is done, reply with one short sentence.";

// Spells out every tool's exact name and arguments for the system prompt. Clients rename MCP
// tools (Claude Code: mcp__<server>__patch_state, OpenCode: <server>_patch_state), and a model
// that calls the short name from the charter gets nothing; Codex only reaches MCP tools from its
// code runtime, where the model never sees their schemas. These few lines keep every agent's
// calls exact. `prefix` is how this agent names the app's tools.
std::string tool_contract(const Json& tools, std::string_view agent_name,
                          std::string_view prefix = "", std::string_view closing = PASS_CLOSING);

// One installed agent, ready to run passes.
// Affinity: one pass at a time; run_pass blocks (never on the GUI thread). close() may be called
// from another thread to stop a running pass.
class Driver {
public:
    virtual ~Driver() = default;
    // The agent's product name ("Claude Code").
    virtual std::string_view name() const = 0;
    // Gets ready: headless CLIs only check their version (they take the system prompt and tools
    // per pass); OpenCode starts `opencode acp` with `personas` (system prompt per primary agent,
    // build and plan) and `tools` as every session's tools. Throws AgentUnavailable.
    virtual void start(LoopbackMcpServer& server, std::map<std::string, std::string> personas = {},
                       const Json& tools = Json::array()) = 0;
    virtual bool healthy() const = 0;
    // Stops what runs and releases the agent; a pass still running ends.
    virtual void close() = 0;
    virtual PassOutcome run_pass(const PassRequest& request) = 0;
    // How this agent names the app's MCP tools ("mcp__openwhisper_cpp__", "openwhisper_cpp_", "").
    virtual std::string tool_prefix() const { return {}; }
    // True for Claude Code, the one agent a pass can fence to one folder (Python's isinstance
    // check on ClaudeCodeDriver). supports_read_folder says whether this install can.
    virtual bool reads_folders() const { return false; }
    // Whether this install can fence Read to a folder; after start().
    virtual bool supports_read_folder() const { return false; }
};

// The driver for `agent`, starting its processes through `launcher`. Agent environment and
// workspace come from installed_agents(). Throws AgentUnavailable ("Unknown agent 'x'").
std::unique_ptr<Driver> make_driver(const InstalledAgent& agent,
                                    std::shared_ptr<ProcessLauncher> launcher);

} // namespace fv::agents
