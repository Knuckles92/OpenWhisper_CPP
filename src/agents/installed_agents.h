#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "agents/agent_config.h"
#include "base/cancel.h"
#include "base/env.h"
#include "base/executor.h"
#include "base/signal.h"
#include "base/subscription.h"
#include "process/agent_process.h"
#include "process/launcher.h"

// Coding agents the user already has installed: Claude Code, Codex, OpenCode
// (services/installed_agents.py). Finds each command-line agent on PATH and in the folders its
// installers use, reads its version and sign-in without starting anything long-lived, and lists
// the models it offers. Every probe has a short timeout and never throws, so a broken install
// reads as "not found" or as a problem to show.
namespace fv::agents {

// What the app knows about one agent before looking for it.
struct AgentSpec {
    std::string_view id;     // MeetingAgentCore value
    std::string_view name;   // product name shown to the user
    std::string_view vendor; // who makes it, for the privacy line
    std::vector<std::string_view> commands;
    std::string_view transport; // "headless" (one process per pass) or "acp"
    std::string_view install_url;
    std::string_view install_hint;
    std::string_view monogram; // two letters for the tile mark
    std::string_view accent;   // tile accent colour, "#RRGGBB"
    std::array<int, 3> min_version;
    std::string_view update_hint;
};

// AGENT_ORDER: Claude Code, Codex, OpenCode.
const std::vector<AgentSpec>& agent_specs();
// AGENT_SPECS[id]; throws std::out_of_range for an unknown id.
const AgentSpec& agent_spec(std::string_view id);
// AGENT_SPECS.get(id).
const AgentSpec* find_agent_spec(std::string_view id);
std::vector<std::string_view> agent_order();

// One agent found on this computer.
struct InstalledAgent {
    std::string id;
    std::filesystem::path path; // the executable that will run
    std::string version;        // as the agent printed it ("2.1.281")
    std::string account;        // how it is signed in ("Claude Team", "ChatGPT"), or ""
    // false: the agent reported no sign-in; nullopt: it can't say without starting a service.
    std::optional<bool> signed_in;
    std::string problem; // why it can't run, or ""

    // True when the app can start passes with it.
    bool usable() const { return problem.empty() && signed_in != false; }
    const AgentSpec& spec() const { return agent_spec(id); }
    bool operator==(const InstalledAgent&) const = default;
};

using ScanResult = std::map<std::string, std::optional<InstalledAgent>>;

// parse_version: the first version number in a text, as a sort key (major, minor, patch, then 0
// for a pre-release and 1 for a release) and as printed. key is nullopt and text "" when there
// is none. Numbers beyond int64 saturate.
struct ParsedVersion {
    std::optional<std::array<std::int64_t, 4>> key;
    std::string text;
};
ParsedVersion parse_version(std::string_view text);

// The sign-in a probe's output shows: (signed_in, account). `code` is the probe's exit code,
// nullopt when it couldn't run (_claude_sign_in, _codex_sign_in after their _run).
std::pair<std::optional<bool>, std::string> claude_sign_in_from(std::optional<int> code,
                                                                std::string_view output);
std::pair<std::optional<bool>, std::string> codex_sign_in_from(std::optional<int> code,
                                                               std::string_view output);
// What to run when the agent reports no sign-in.
std::string sign_in_hint(std::string_view agent_id);

// The agents on this computer, with the session's cached scan and lookups.
// Affinity: thread-safe. scan_agents, resolve_agent, find_agent, with_sign_in and list_models
// block (they start the agents) and must not run on the GUI thread; known_agent and cached_scan
// never block.
class InstalledAgents {
public:
    // base_env gives the user's environment for children; the app passes
    // ProcessEnv::instance().external_env, tests a fixed map. It is also where PATH, PATHEXT,
    // LOCALAPPDATA, APPDATA and the agents' configuration variables are read. home is the
    // user's home folder; temp_dir holds the agent workspace (empty: the system's temp folder).
    InstalledAgents(std::shared_ptr<ProcessLauncher> launcher,
                    std::function<std::map<std::string, std::string>()> base_env,
                    std::filesystem::path home, std::filesystem::path temp_dir = {});
    // Stops the background lookups known_agent started and waits for them: each ends after the
    // probe it is running, at most one probe's timeout (Python's daemon threads never held up
    // exit). A subclass that overrides find_agent, with_sign_in or list_models calls
    // stop_lookups() (or wait_for_lookups()) in its own destructor, since a lookup still running
    // would call into it.
    virtual ~InstalledAgents();
    InstalledAgents(const InstalledAgents&) = delete;
    InstalledAgents& operator=(const InstalledAgents&) = delete;

    // Every supported agent and its sign-in, in AGENT_ORDER; cached for the session until
    // `refresh`. Blocking, seconds at most. Each scan forgets the lookups and notifies every id.
    ScanResult scan_agents(bool refresh = false);
    // The last scan, or nullopt before the first one. Never blocks.
    std::optional<ScanResult> cached_scan() const;
    // (known, agent) from the last scan or lookup. Never blocks: when nothing is known, or the
    // agent wasn't usable, one lookup starts in the background (at most one per agent at a
    // time) and the lookup listeners hear if the answer changes.
    std::pair<bool, std::optional<InstalledAgent>> known_agent(std::string_view id);
    // The agent a run will use: a usable cached agent whose file still exists as it is, else a
    // fresh lookup (kept until the next scan when usable). Blocking when it has to look.
    std::optional<InstalledAgent> resolve_agent(std::string_view id);
    // Called with an agent id, on `executor`, when what is known about it changes.
    Subscription add_lookup_listener(Executor& executor,
                                     std::function<void(std::string agent_id)> listener);
    // Joins the lookups known_agent started (tests).
    void wait_for_lookups();
    // From now on every probe fails at once without starting anything, the probes running end
    // (their trees killed) within detail::PROBE_CANCEL_POLL, and no lookup records or reports
    // its answer; then joins the lookups. For shutdown (Quit's CancelRoot step, before the io
    // strand is waited for): the instance answers nothing useful afterwards.
    void stop_lookups();

    // Every existing executable that could be the agent, PATH first, each real file once.
    std::vector<std::filesystem::path> candidate_paths(std::string_view id) const;
    // The newest working install, without its sign-in; nullopt when no candidate answers
    // --version.
    virtual std::optional<InstalledAgent> find_agent(std::string_view id);
    // The agent with its sign-in, where it can be read safely (never OpenCode's).
    virtual InstalledAgent with_sign_in(const InstalledAgent& agent);
    // The agent's models, its own default ("") first. OpenCode's take an ACP handshake of up to
    // timeout_s; a failed lookup still gives the default.
    virtual std::vector<AgentModel> list_models(const InstalledAgent& agent,
                                                double timeout_s = 20.0);

    // The user's environment minus the app's internal tokens, plus `extra`.
    std::map<std::string, std::string>
    agent_child_env(const std::map<std::string, std::string>& extra = {}) const;
    // <temp>\<product in lower case>-agent-workspace, created if needed: an empty folder outside
    // any repository that agents run in. Throws std::filesystem::filesystem_error when it
    // can't be created.
    std::filesystem::path agent_workspace_dir() const;

    const AgentConfig& config() const { return config_; }
    const std::shared_ptr<ProcessLauncher>& launcher() const { return launcher_; }
    const std::filesystem::path& home() const { return home_; }
    // One variable of base_env, matched as the system matches names.
    std::optional<std::string> env(std::string_view name) const;

private:
    // _run: a short probe in the workspace with the agent environment; {nullopt, ""} on any
    // failure.
    ProbeResult run(const std::vector<std::string>& argv, double timeout_s) const;
    std::optional<std::filesystem::path> which(std::string_view command) const;
    void notify(const std::vector<std::string>& ids);

    std::shared_ptr<ProcessLauncher> launcher_;
    std::function<std::map<std::string, std::string>()> base_env_;
    std::filesystem::path home_;
    std::filesystem::path temp_dir_;
    AgentConfig config_;

    mutable std::mutex mutex_;
    std::optional<ScanResult> scan_result_;
    // What resolve_agent found on its own since the last scan (nullopt: missing).
    ScanResult looked_up_;
    std::set<std::string> looking_up_;
    std::map<std::string, std::thread> lookups_;
    std::atomic<bool> stopping_{false};
    // Cancelled by stop_lookups: the token every probe waits under.
    CancelSource stop_;
    Signal<std::string> changed_;
};

// Constructs the desktop registry. Lookup and sign-in probes run only when requested.
std::shared_ptr<InstalledAgents> make_installed_agents(std::shared_ptr<ProcessLauncher> launcher,
                                                       const EnvReader& env);

// The app's instance (built by the composition), and the tests'.
void set_installed_agents(std::shared_ptr<InstalledAgents> agents);
// Throws std::logic_error when none is set.
InstalledAgents& installed_agents();
// The set instance, or null.
std::shared_ptr<InstalledAgents> installed_agents_if_set();

// Short model name for a summary line ("Haiku", "gpt-6-sol"); an empty model is the configured
// default's last "/" segment, or "default model". Without a config, the set instance's (else
// this process's environment).
std::string model_display_name(const AgentConfig& config, std::string_view agent_id,
                               std::string_view model, const std::vector<AgentModel>& models = {});
std::string model_display_name(std::string_view agent_id, std::string_view model,
                               const std::vector<AgentModel>& models = {});
// "Claude Code · Haiku": the agent and model a run will use; "" for an unknown agent.
std::string describe_choice(const AgentConfig& config, std::string_view agent_id,
                            std::string_view model);
std::string describe_choice(std::string_view agent_id, std::string_view model);
// The first entry of every model list, and Codex's thinking levels per model, from the set
// instance's config (else this process's environment).
std::string default_model_label(std::string_view agent_id);
ModelEfforts codex_model_efforts();

// Claude Code's model aliases, after its default.
const std::vector<AgentModel>& claude_models();

} // namespace fv::agents
