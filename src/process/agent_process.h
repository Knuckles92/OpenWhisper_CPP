#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "process/launcher.h"

// Owning an installed agent's descendants until its request ends (services/agent_process.py,
// installed_agents.py _run and kill_process_tree).
namespace fv {

struct ProbeResult {
    std::optional<int> code; // nullopt: it couldn't start, or didn't finish in time
    std::string text;        // stdout then stderr, decoded as UTF-8 with errors replaced
};

// A short captured command (a version or sign-in probe): stdin from NUL, no window, in a job,
// with child_env() in `cwd`. Waits for the exit and both pipes' ends until `timeout_s`; the
// process tree is killed afterwards whatever happened. Any failure gives {nullopt, ""}.
ProbeResult run_agent_probe(const std::vector<std::string>& argv, double timeout_s,
                            const std::filesystem::path& cwd = {},
                            ProcessLauncher* launcher = nullptr);

// Stops the process and everything it started, then waits up to `wait_s` for it; one still
// running is killed again and waited for one more second. Never throws; repeating it is harmless.
void kill_process_tree(Process& process, double wait_s = 3.0);

} // namespace fv
