#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "base/cancel.h"
#include "process/agent_process.h"
#include "process/launcher.h"

namespace fv::agents::detail {

// run_agent_probe (process/agent_process.h) with the environment given instead of the
// process-wide child_env(): stdin from NUL, no window, in a job, in `cwd`. Waits for the exit
// and both pipes' ends until `timeout_s`; the tree is killed afterwards whatever happened. The
// text is stdout then stderr in text mode. Any failure gives {nullopt, ""}, and so does
// `cancel` firing, which ends the wait within PROBE_CANCEL_POLL (Quit stops a scan's probes).
ProbeResult run_probe(ProcessLauncher& launcher, const std::vector<std::string>& argv,
                      double timeout_s, const std::filesystem::path& cwd,
                      std::map<std::string, std::string> env, const CancelToken& cancel = {});

inline constexpr std::chrono::milliseconds PROBE_CANCEL_POLL{100};

} // namespace fv::agents::detail
