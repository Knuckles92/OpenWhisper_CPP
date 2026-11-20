#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "agents/acp.h"
#include "agents/drivers.h"
#include "agents/installed_agents.h"
#include "agents/loopback_mcp.h"
#include "base/json.h"
#include "process/launcher.h"

// The drivers behind make_driver (drivers.py): the headless supervision Claude Code and Codex
// share, each CLI's command line and event reading, and OpenCode over ACP. Tests reach the
// pieces Python's tests reach (build, handle_event, finish, the overlay, permission answers).
namespace fv::agents::detail {

// stderr lines a headless pass keeps, and the characters kept of each.
inline constexpr std::size_t PASS_STDERR_TAIL = 20;
inline constexpr std::int64_t PASS_STDERR_LINE_CHARS = 400;
// How long a --help, features or MCP listing probe may take, in seconds.
inline constexpr double PASS_HELP_TIMEOUT_S = 15.0;
// How long a headless run may linger after reporting its result.
inline constexpr std::chrono::milliseconds DONE_GRACE{5000};
// How often the supervisor wakes to check deadlines while the agent runs.
inline constexpr std::chrono::milliseconds SUPERVISE_POLL{250};
// Waits for a finished headless process, and for its stderr reader.
inline constexpr std::chrono::milliseconds EXIT_WAIT{5000};
inline constexpr std::chrono::milliseconds STDERR_JOIN{1000};
// ACP request timeouts for OpenCode session configuration and cleanup, in seconds.
inline constexpr double ACP_CONFIG_TIMEOUT_S = 30.0;
inline constexpr double ACP_DELETE_TIMEOUT_S = 15.0;
inline constexpr double ACP_SESSION_TIMEOUT_S = 60.0;
// How long OpenCode may take to list the app's tools after its first session opens.
inline constexpr double MCP_READY_TIMEOUT_S = 20.0;
// OpenCode registers MCP tools a moment after listing them; a prompt sent in between reaches the
// model with no tools, and Claude or DeepSeek then write their calls out as text. Its command
// list update follows the registration, and a short grace covers versions that send none.
inline constexpr double TOOLS_REGISTERED_WAIT_S = 3.0;
inline constexpr std::chrono::milliseconds TOOLS_REGISTERED_GRACE{500};
// How long closing waits for session deletes already on their way, so a meeting that ends right
// after a pass doesn't leave it in OpenCode's history.
inline constexpr std::chrono::milliseconds DELETE_ON_CLOSE{2000};

// _help_text: the output of a read-only probe (`--help`, `exec --help`, `features list`),
// stdout then stderr, cached per (path, version, args) even when empty; `mcp list --json` is run
// again every time, since users add MCP servers while the app runs. "" when it can't run.
std::string driver_help(ProcessLauncher& launcher, const InstalledAgent& agent,
                        const std::vector<std::string>& args);
// Forgets the cached probe outputs (tests).
void forget_driver_help();
// How a driver reads probe output; tests pass canned texts, as Python's tests patch _help_text.
using HelpText =
    std::function<std::string(const InstalledAgent& agent, const std::vector<std::string>& args)>;

// The environment variable Codex reads our bearer token from: identity().env_prefix +
// "MCP_TOKEN".
std::string mcp_token_env();

// What a headless pass has read from the agent so far.
struct RunState {
    std::string text;
    std::string error;
    std::string fatal;
    bool done = false;
    Json usage = Json::object();
};

// ": <last non-blank stderr line, 240 characters>", or "".
std::string tail_detail(std::string_view stderr_tail);

// Each pass is a fresh CLI process; subclasses speak one CLI.
// Affinity: one pass at a time; close() from any thread.
class HeadlessDriver : public Driver {
public:
    HeadlessDriver(InstalledAgent agent, std::shared_ptr<ProcessLauncher> launcher,
                   HelpText help = {});
    ~HeadlessDriver() override;

    std::string_view name() const override;
    // Gets ready; headless CLIs take their system prompt and tools per pass.
    void start(LoopbackMcpServer& server, std::map<std::string, std::string> personas = {},
               const Json& tools = Json::array()) override;
    // Throws AgentUnavailable when this CLI cannot run passes.
    virtual void check_ready() {}
    bool healthy() const override;
    void close() override;
    PassOutcome run_pass(const PassRequest& request) override;

    // Command line and extra environment for one pass; writes its files into `workdir`.
    virtual std::pair<std::vector<std::string>, std::map<std::string, std::string>>
    build(const PassRequest& request, std::string_view url, std::string_view token,
          const std::filesystem::path& workdir) = 0;
    // One JSON event line the CLI printed. May throw on a malformed event: the reader logs it.
    virtual void handle_event(const Json& event, RunState& state, const PassRequest& request) = 0;
    virtual PassOutcome finish(const RunState& state, std::optional<int> returncode,
                               std::string_view stderr_tail) = 0;

    const InstalledAgent& agent() const { return agent_; }

protected:
    std::string help(const std::vector<std::string>& args) const;

private:
    struct Supervision;
    PassOutcome supervise(Process& process, const PassRequest& request,
                          const std::shared_ptr<Supervision>& supervision);
    std::optional<PassOutcome> watch(Process& process, const PassRequest& request,
                                     Supervision& supervision);

    InstalledAgent agent_;
    std::shared_ptr<ProcessLauncher> launcher_;
    HelpText help_;
    mutable std::mutex mutex_;
    LoopbackMcpServer* server_ = nullptr;
    bool closed_ = false;
    std::vector<std::shared_ptr<Process>> procs_;
};

// `claude -p` with only the app's MCP tools.
class ClaudeCodeDriver final : public HeadlessDriver {
public:
    using HeadlessDriver::HeadlessDriver;

    void check_ready() override;
    bool supports_read_folder() const override;
    bool reads_folders() const override { return true; }
    std::string tool_prefix() const override;
    std::pair<std::vector<std::string>, std::map<std::string, std::string>>
    build(const PassRequest& request, std::string_view url, std::string_view token,
          const std::filesystem::path& workdir) override;
    void handle_event(const Json& event, RunState& state, const PassRequest& request) override;
    PassOutcome finish(const RunState& state, std::optional<int> returncode,
                       std::string_view stderr_tail) override;

private:
    bool has(std::string_view flag) const;
    std::string flags_; // the --help text check_ready read
};

// `codex exec --json` with the shell off and only the app's tools.
class CodexDriver final : public HeadlessDriver {
public:
    using HeadlessDriver::HeadlessDriver;

    void check_ready() override;
    std::pair<std::vector<std::string>, std::map<std::string, std::string>>
    build(const PassRequest& request, std::string_view url, std::string_view token,
          const std::filesystem::path& workdir) override;
    void handle_event(const Json& event, RunState& state, const PassRequest& request) override;
    PassOutcome finish(const RunState& state, std::optional<int> returncode,
                       std::string_view stderr_tail) override;

    // _mcp_servers_to_disable: the user's other MCP servers, refreshed per call.
    std::vector<std::string> mcp_servers_to_disable() const;

private:
    std::string exec_help_;
    std::vector<std::string> disable_{"shell_tool"};
};

// Claude Code's events (drivers.py module functions).
void emit_stream_block(const Json& event, const PassRequest& request);
Json claude_usage(const Json& event);
void on_claude_result(const Json& event, RunState& state, const PassRequest& request);
std::string claude_error(std::string_view message);
// Codex's events.
void on_codex_item(std::string_view kind, const Json& item, RunState& state,
                   const PassRequest& request);
Json codex_usage(const Json& usage);
std::string codex_error(std::string_view message);
// _CODEX_DISABLE: the optional tool features a pass switches off when they are on.
const std::vector<std::string_view>& codex_disable_names();
// _features_to_disable over a `features list` output.
std::vector<std::string> codex_features_to_disable(std::string_view listing);
// The names _mcp_servers_to_disable reads from an `mcp list --json` output; throws
// AgentUnavailable when the listing cannot be trusted.
std::vector<std::string> codex_servers_from(std::string_view listing);

// The OPENCODE_CONFIG_CONTENT overlay: both primary agents locked down (the app's charter, or
// the tool-free text assistant, then their persona) and the app's MCP server with code mode off.
Json opencode_overlay(bool tool_free, const std::map<std::string, std::string>& personas,
                      std::string_view url, std::string_view token);
// _on_request: the answer to OpenCode's request during a pass. A permission is granted only for
// the app's tools; anything else throws AcpError (-32601).
Json opencode_request(const std::string& method, const Json& params);
// _pass_model: (model, error); model "" keeps the session's.
std::pair<std::string, std::string> opencode_pass_model(const Json& session, std::string_view model,
                                                        std::string_view user_default);
// _failed and _outcome.
PassOutcome opencode_failed(const AcpError& error, bool canceled);
PassOutcome opencode_outcome(const Json& result);
// _image_block: an ACP image block holding the PNG at `path`. Throws filesystem_error.
Json image_block(const std::filesystem::path& path);

// The user's OpenCode, over ACP, with the app's agent layered on top. Their providers,
// credentials and models stay in charge; the overlay (OPENCODE_CONFIG_CONTENT, this process only)
// replaces the build and plan agents' charters and permissions and adds the app's MCP server
// with code mode off. Each pass is a new session in the persona it needs, deleted afterwards so
// passes do not fill the user's history. The instructions are the session's system prompt: sent
// in the user turn next to a written tool list, Claude and DeepSeek models wrote their tool calls
// out as text instead of making them.
// Affinity: one pass at a time; ACP callbacks arrive on the connection's reader thread.
class OpenCodeDriver final : public Driver {
public:
    OpenCodeDriver(InstalledAgent agent, std::shared_ptr<ProcessLauncher> launcher);
    ~OpenCodeDriver() override; // close()

    std::string_view name() const override;
    void start(LoopbackMcpServer& server, std::map<std::string, std::string> personas = {},
               const Json& tools = Json::array()) override;
    bool healthy() const override;
    void close() override;
    PassOutcome run_pass(const PassRequest& request) override;
    std::string tool_prefix() const override;

    // The overlay this driver starts OpenCode with.
    Json overlay() const;
    std::vector<std::string> argv() const;
    // The endpoint every session's tool calls reach (null before start and after close).
    std::shared_ptr<McpEndpoint> router() const;

private:
    struct ActivePass;
    struct Delete;
    std::pair<std::string, bool> route(std::string_view name, const Json& args, const Json& meta);
    void on_notification(const std::string& method, const Json& params);
    void spawn();
    void warm_up();
    void await_tools();
    std::string prepare(AcpConnection& conn, const Json& session, const std::string& session_id,
                        const PassRequest& request);
    std::string set_persona(AcpConnection& conn, const Json& session, const std::string& session_id,
                            const PassRequest& request);
    void forget(const std::shared_ptr<AcpConnection>& conn, const std::string& session_id);

    InstalledAgent agent_;
    std::shared_ptr<ProcessLauncher> launcher_;
    std::string user_default_;
    mutable std::mutex mutex_;
    LoopbackMcpServer* server_ = nullptr;
    std::shared_ptr<McpEndpoint> router_;
    std::shared_ptr<AcpConnection> conn_;
    Json capabilities_ = Json::object();
    bool takes_images_ = false;
    std::map<std::string, std::shared_ptr<ActivePass>> passes_;
    std::map<std::string, std::string> personas_;
    bool closed_ = false;
    bool ready_ = false;
    bool tool_free_ = false;
    Event commands_updated_;
    std::vector<std::shared_ptr<Delete>> deletes_;
};

} // namespace fv::agents::detail
