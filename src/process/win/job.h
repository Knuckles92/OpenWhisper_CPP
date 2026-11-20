#pragma once

#include <cstdint>

#include "process/win/wide.h"

namespace fv::process_win {

// A job whose processes all end when its last handle closes (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE),
// assigned to a child before it runs (agent_process.py _WindowsJob). nullptr with the error in
// `error` when it can't be made.
HANDLE create_kill_on_close_job(std::uint32_t& error);

// Clears every limit, kill-on-close included, so closing the handle afterwards leaves the job's
// processes running (programs.py _disown). False when Windows refused.
bool clear_job_limits(HANDLE job);

// Ends `pid` and its descendants by parent process id, as taskkill /PID <pid> /T /F does: for a
// process started without a job.
void terminate_tree(std::uint32_t pid);

// Whether the process is in any job (IsProcessInJob).
bool in_job(HANDLE process);

} // namespace fv::process_win
