#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// services/backup_startup.py:44, 196-236: the restart that applies a staged restore. The app
// exits with restart_exit_code once a restore is staged; main then starts a fresh copy.
namespace fv::backup {

// RESTART_EXIT_CODE.
inline constexpr int restart_exit_code = 73;

// Starts a program detached from this process, with no console window (implemented over
// process/detached.h's start_detached by the app).
class ProcessStarter {
public:
    virtual ~ProcessStarter() = default;
    // The failure as a sentence.
    virtual std::expected<void, std::string> start(const std::filesystem::path& exe,
                                                   std::span<const std::string> args) = 0;
};

// restart_command: the installed executable alone, as UTF-8. Never the original arguments, so an
// update or restore flag of this run is not repeated.
std::vector<std::string> restart_command(const std::filesystem::path& exe);

// spawn_restart: starts restart_command(exe) through `starter`. Call it after the app has
// closed and released its data lease.
std::expected<void, std::string> spawn_restart(const std::filesystem::path& exe,
                                               ProcessStarter& starter);

} // namespace fv::backup
