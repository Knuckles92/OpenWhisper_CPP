#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "backup/restart.h"

namespace fv {

// What main.py does once main() has returned (main.py:398-416), for a run that opened the app.
// Exit code backup::restart_exit_code (a restore or import was staged, or the code-change offer
// was taken): `releaseGate` lets the update launch gate's app mutexes go, because the fresh
// process takes them without waiting, then backup::spawn_restart starts `exe` alone through
// `starter`. Returns 0 once it started; 1 after showStartupError has shown "Could not restart
// <product>: <reason> Please start <product> again manually." (through `errorBox` too). Any other
// code, and every code of a run that did not open the app, comes back unchanged and nothing is
// released or started.
int finishRun(int code, bool opensApp, const std::function<void()>& releaseGate,
              const std::filesystem::path& exe, backup::ProcessStarter& starter,
              const std::function<void(const std::string&)>& errorBox);

} // namespace fv
