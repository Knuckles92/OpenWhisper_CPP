#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "agents/drivers.h"
#include "agents/installed_agents.h"
#include "base/cancel.h"
#include "base/json.h"
#include "net/reactor.h"
#include "process/launcher.h"

// One-off tasks through the user's installed agent (meeting/agent/installed/core.py:323-439): a
// tool server and the agent's driver are started for one pass and stopped after it. The closing
// reply is the task's result. Each call blocks for the whole run (never on the GUI thread).
namespace fv::agents {

// Where meeting intelligence picks its agent, as "not installed" names it.
std::string_view meeting_agent_setting(); // "Settings → Meeting Mode → Intelligence"
// Effort for one-off tasks that name none.
inline constexpr std::string_view FINAL_EFFORT = "medium";
// The silence limit of a custom report's pass (meeting/agent/base.py), in seconds.
inline constexpr double CONSOLIDATION_STALL_S = 300.0;

// The runnable install of `agent_id` (installed_agents().resolve_agent), or AgentUnavailable
// with the sentence to show: unknown, not installed, its problem, or signed out.
InstalledAgent installed_agent(std::string_view agent_id);

// What a task's driver is made with; tests make their own (Python's tests patch make_driver).
using DriverFactory = std::function<std::unique_ptr<Driver>(const InstalledAgent&)>;
// The reactor every task's tool server runs on and the launcher its agent starts with, set by
// the composition; `make` empty uses make_driver with that launcher. The reactor must outlive
// every task, and clear_agent_task_services() runs before it is destroyed.
void set_agent_task_services(net::Reactor& reactor, std::shared_ptr<ProcessLauncher> launcher,
                             DriverFactory make = {});
void clear_agent_task_services();

// A task in which the agent reads the files in `folder`, and only those. Only Claude Code can be
// held to one folder, so only it runs these; the others run run_viewing_task. Throws
// AgentUnavailable when the agent cannot run or cannot be fenced in, std::logic_error when the
// services are not set.
PassOutcome run_reading_task(std::string_view agent_id, std::string_view model,
                             std::string_view system_prompt, std::string_view user_prompt,
                             const std::filesystem::path& folder, double timeout_s, double stall_s,
                             std::string_view effort = FINAL_EFFORT, const CancelToken& = {});
// A task in which the agent sees `images` beside the prompt, and no files: for Codex and
// OpenCode, which can't be fenced to a folder. They keep the meeting lockdown (no shell, no file
// tools, none of the user's MCP servers) and get everything they need in the prompt itself.
// Throws AgentUnavailable when the agent cannot run, or is Claude Code (which reads the folder).
PassOutcome run_viewing_task(std::string_view agent_id, std::string_view model,
                             std::string_view system_prompt, std::string_view user_prompt,
                             const std::vector<std::filesystem::path>& images, double timeout_s,
                             double stall_s, std::string_view effort = FINAL_EFFORT,
                             const CancelToken& = {});
// A self-contained task (a custom report, agent cleanup) with the task's own read-only `tools`,
// answered by `handler` (its meta argument is always an empty object). `closing` says what the
// final reply must be, since that reply is the task's result. Throws AgentUnavailable.
PassOutcome run_agent_task(std::string_view agent_id, std::string_view model,
                           std::string_view system_prompt, std::string_view user_prompt,
                           const Json& tools, ToolHandler handler, double timeout_s,
                           std::string_view closing, std::string_view effort = FINAL_EFFORT,
                           const CancelToken& = {});

} // namespace fv::agents
