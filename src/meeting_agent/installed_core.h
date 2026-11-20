#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "agents/agent_tasks.h"
#include "agents/drivers.h"
#include "agents/loopback_mcp.h"
#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "meeting_agent/interfaces.h"
#include "net/reactor.h"
#include "process/launcher.h"

// meeting/agent/installed/core.py: an AgentCore for meetings run by the user's installed coding
// agent (Claude Code, Codex, OpenCode). Each pass is one agent run: the same host-written prompts
// the built-in loop uses plus a tool contract, with the meeting tools as the only tools, served
// by a loopback MCP server. Every tool call is answered by run_tool under the pass's own
// ToolScope, so a notes pass can only touch notes and a polish pass only transcript text, and
// every op is validated by the state-patch layer. Runs are slower than the built-in loop and
// bill the user's own plan, so the core asks the scheduler for INSTALLED_AGENT_CADENCE.
namespace fv::meeting_agent {

// A live pass's hard wall and silence limit, in seconds; a polish pass gets the longer of this
// wall and the polish budget.
inline constexpr double INSTALLED_LIVE_TIMEOUT_S = 120.0;
inline constexpr double INSTALLED_LIVE_STALL_S = 75.0;
// The same progress line reaches the finalization card at most this often (seconds).
inline constexpr double INSTALLED_PROGRESS_MIN_INTERVAL_S = 1.0;

// Affinity: thread-safe; passes block their caller (never the GUI thread), cancel() and
// shutdown() may come from any thread.
class InstalledAgentCore final : public AgentCore {
public:
    // `agent_id` is a MeetingAgentCore value of an installed agent. `settings` reads the current
    // settings (the live and final thinking levels). The tool server runs on `reactor`, which
    // must outlive the core, and the agent starts through `launcher`; `make` replaces
    // agents::make_driver (tests). `clock` spaces the repeated progress lines.
    InstalledAgentCore(std::string agent_id, std::function<Json()> settings, net::Reactor& reactor,
                       std::shared_ptr<ProcessLauncher> launcher, agents::DriverFactory make = {},
                       const Clock& clock = system_clock());
    // shutdown().
    ~InstalledAgentCore() override;
    InstalledAgentCore(const InstalledAgentCore&) = delete;
    InstalledAgentCore& operator=(const InstalledAgentCore&) = delete;

    const std::string& agent_id() const { return agent_id_; }
    // The agent's product name ("Claude Code").
    std::string name() const;

    // Finds the agent, starts the tool server and readies the driver. Throws
    // agents::AgentUnavailable when the agent is missing, too old or signed out (the message says
    // what to do), and what the driver's start throws.
    void initialize(const AgentConfig& config, std::shared_ptr<AgentToolHost> tools) override;
    AgentResult checkpoint(const CheckpointPayload& payload) override;
    AgentResult consolidate(const CheckpointPayload& payload) override;
    void cancel() override;
    bool is_healthy() const override;
    void shutdown() override;
    std::optional<CheckpointCadence> cadence() const override;
    void set_progress_callback(std::function<void(const std::string&)> callback) override;
    void set_activity_callback(std::function<void(const AgentActivity&)> callback) override;

private:
    AgentResult run(const CheckpointPayload& payload, double timeout_s, double stall_s,
                    const std::string& effort);
    void on_event(std::string_view kind, std::string_view tool, const std::string& pass_kind);

    const std::string agent_id_;
    const std::function<Json()> settings_;
    net::Reactor& reactor_;
    const std::shared_ptr<ProcessLauncher> launcher_;
    const agents::DriverFactory make_;
    const Clock& clock_;

    mutable std::mutex mutex_;
    std::optional<AgentConfig> config_;
    std::shared_ptr<AgentToolHost> tools_;
    // A pass holds both for its whole run: the driver needs the server until its first close.
    std::shared_ptr<agents::LoopbackMcpServer> server_;
    std::shared_ptr<agents::Driver> driver_;
    std::map<std::string, std::string> personas_;
    std::set<std::shared_ptr<CancelSource>> active_;
    bool initialized_ = false;
    bool shut_down_ = false;
    std::string live_effort_ = "low";
    std::string final_effort_ = std::string(agents::FINAL_EFFORT);
    std::function<void(const std::string&)> progress_cb_;
    std::function<void(const AgentActivity&)> activity_cb_;
    std::string last_progress_;
    std::optional<std::chrono::steady_clock::time_point> last_progress_at_;
    bool progress_cb_warned_ = false;
    bool activity_cb_warned_ = false;
};

} // namespace fv::meeting_agent
