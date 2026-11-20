#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agents/installed_agents.h"
#include "base/cancel.h"
#include "process/launch.h"

// Agent actions (services/actions/agents.py): hand words to an installed coding agent, or ask it.
//
// A handoff opens a visible session the user drives: the agent app's documented deep link when
// its handler is registered (links only prefill; the user sends), else a new terminal running
// the CLI with the words as one argv element. An ask runs the CLI headless with no tools in an
// empty workspace and returns its answer. Words, prompts, answers and paths are never logged.
//
// Every function here works on the set InstalledAgents instance (installed_agents()), which
// gives the launcher, the user's environment and home folder, and the agent workspace.
namespace fv::agents {

// The whole system prompt of an ask.
inline constexpr std::string_view ASK_INSTRUCTION =
    "Answer briefly in plain text. You can't use tools or files.";
// Claude Desktop cuts q near this many characters; longer words go elsewhere.
inline constexpr std::size_t CLAUDE_APP_TEXT_MAX = fv::CLAUDE_APP_TEXT_MAX;
// Claude Code's own claude-cli://open handler takes at most this many characters.
inline constexpr std::size_t CLAUDE_CLI_TEXT_MAX = 5'000;
// A link travels on the handler's command line.
inline constexpr std::size_t LINK_MAX_CHARS = fv::LINK_MAX_CHARS;
// The longest an ask runs by default, in seconds.
inline constexpr double ASK_TIMEOUT_S = 120.0;

// Where a handoff opens (AgentSurface): an app link when its handler is registered, else a
// terminal; app links only; a terminal only.
inline constexpr std::string_view SURFACE_AUTO = "auto";
inline constexpr std::string_view SURFACE_APP = "app";
inline constexpr std::string_view SURFACE_TERMINAL = "terminal";

// A handoff that did not open; what() is the sentence to show.
class HandoffError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Which agents an action can use were found by the last scan, {agent_id: found}; empty before
// the first scan or without a set instance. Never scans.
std::map<std::string, bool> available();

struct HandoffRequest {
    std::string agent_id; // claude_code, codex or opencode_cli
    std::string text;     // percent-encoded in links, one argv element otherwise
    std::string surface = std::string(SURFACE_AUTO);
    // An absolute working folder, or empty for the agent's default (home).
    std::filesystem::path folder;
    // A persistent, empty folder for this session under ~/<product> Agent Tasks instead of
    // `folder`. Codex app links register it as a local project.
    bool new_folder = false;
};

// Opens a visible session of the agent with the words ready and returns what to show ("Opened
// Codex"). Throws HandoffError with the user's sentence when it can't. Blocking (registry
// lookups, Explorer, a lookup of the agent); never on the GUI thread.
std::string handoff(const HandoffRequest& request);

// Every app link that can take the words, best first, each "<scheme>:..."; links that would cut
// the words short are left out. An empty `folder` names none, except that the claude-cli link
// always names one: `home`, or the set instance's home folder when `home` is empty.
std::vector<std::string> app_links(std::string_view agent_id, std::string_view text,
                                   const std::filesystem::path& folder = {},
                                   const std::filesystem::path& home = {});

// The command line that starts the agent interactively with the words: `command` is the
// executable, or the command that starts it (node.exe and its script, see terminal_command).
// The words are always one element, after anything that would let the CLI read them as its own
// options or commands.
std::vector<std::string> terminal_argv(std::string_view agent_id, std::vector<std::string> command,
                                       std::string_view text);
std::vector<std::string> terminal_argv(std::string_view agent_id, const std::filesystem::path& cmd,
                                       std::string_view text);

// How a terminal starts the agent without cmd.exe in between. A .cmd or .bat (npm's shim on
// Windows) would pass the words through cmd.exe: an .exe install of the same agent is used
// instead even when the shim is newer, else the shim is resolved to what it runs
// (npm_shim_command). Only when neither works is the shim returned, which open_in_terminal
// refuses with its own explanation. Blocking (looks at files).
std::vector<std::string> terminal_command(const InstalledAgent& agent);

// The command an npm .cmd shim runs, without cmd.exe: node.exe (beside it, else on the user's
// PATH) with a script of the package, or, without Node, a native .exe the package ships for this
// platform (Codex's vendor\<target>\bin\codex.exe). nullopt when the file is not npm's shim of a
// Node script or what it runs is missing.
std::optional<std::vector<std::string>> npm_shim_command(const std::filesystem::path& shim);

struct AskResult {
    bool ok = false;
    std::string answer;   // the answer, stripped, when ok
    std::string message;  // what to show ("Codex answered", or why there is no answer)
    bool stopped = false; // `cancel` stopped it
};

// Asks the agent with no tools: Claude Code and Codex headless, the question on stdin; OpenCode
// over ACP. `model` is the agent's model id or alias ("" keeps its default). The agent and every
// process it started are killed after max(1, timeout_s) seconds or when `cancel` fires.
// Blocking; never on the GUI thread. Throws std::filesystem::filesystem_error when the temporary
// folder can't be written, and py::PyError where Python's own reading of malformed agent output
// raises (for example .get on a value that is not a dict); the caller treats both as a failed
// action.
AskResult ask(std::string_view agent_id, std::string_view text, std::string_view model = {},
              double timeout_s = ASK_TIMEOUT_S, const CancelToken& cancel = {});

// Syntax accepted for an explicit model ID on the installed agent command line.
bool valid_agent_model_id(std::string_view model);

// What closes the cleanup instructions sent to an agent: OpenWhisper's closing sentence
// (transcript_cleanup.py, _cleanup_with_agent), then the tool-free statement its tool contract
// makes, since OpenWhisper_CPP's cleanup path sends no tool contract.
inline constexpr std::string_view kCleanupClosing =
    "\n\nReturn only the finished text requested by the instructions, without commentary. "
    "No tools are available.";

// Tool-free transcript cleanup through the selected authenticated CLI. Failures are sanitized.
AskResult ask_for_cleanup(std::string_view agent_id, std::string_view text,
                          std::string_view system_prompt, std::string_view model,
                          std::string_view effort, double timeout_s, const CancelToken& cancel);

} // namespace fv::agents
