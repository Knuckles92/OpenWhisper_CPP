#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// A child that outlives this process: the setup that replaces the app, the restart after a
// restore (services/app_update_apply.py: _launch_exe, _start_detached). The general launcher with
// job objects and pipes is process/launcher.h.
namespace fv {

// Starts `program` with `args` (UTF-8, quoted as subprocess.list2cmdline quotes them) in `cwd`
// (empty: the program's folder) and returns its process id without waiting for it. The child
// inherits this environment but no handle. On Windows it is started DETACHED_PROCESS |
// CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, with the DLL search path reset
// (SetDllDirectoryW(NULL)) around the launch so it does not load this process's private DLL
// folder; elsewhere in a new session. Failure: "Could not start <file name>."
// Affinity: thread-safe; it changes the process-wide DLL directory for the moment of the launch.
std::expected<std::uint32_t, std::string> start_detached(const std::filesystem::path& program,
                                                         std::span<const std::string> args,
                                                         const std::filesystem::path& cwd = {});

// subprocess.list2cmdline: one Windows command line from an argument list, as the Microsoft C
// runtime parses it back.
std::string list2cmdline(std::span<const std::string> args);

} // namespace fv
