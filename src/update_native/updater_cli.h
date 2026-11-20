#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "update_native/journal.h"

// fiorivoce-updater's command line (scripts/updater_helper.py: main, _run_setup). The program in
// src/updater is only main(): everything it decides is here, so tests can link it.
namespace fv::update_native {

// What the installer's temporary helper knows when it runs a setup action.
struct SetupSession {
    // The temporary folder Inno extracted the helper and the incoming manifest into. The
    // protocol's own files (setup-origin.json, setup-error.txt) are written there.
    std::filesystem::path helper_dir;
    // Whether the per-user registration names `install`. Asked inside the protocol, so a failure
    // to find out reaches setup-error.txt. Empty: not registered.
    std::function<bool(const std::filesystem::path& install)> registered;
    // Retires the native transactions of a registered installation before its files are backed
    // up (retire_transactions). May be empty.
    std::function<void()> retire_transactions;
};

// --setup-action prepare|finish|rollback --app-dir <install>: the backup protocol of
// setup_backup.h around an Inno installation. prepare records where it was asked to work and
// whether that folder was registered; finish and rollback refuse another folder. Returns 0, or 2
// for an action it does not know. A failure is written to <helper_dir>/setup-error.txt, which
// the installer shows, and rethrown.
int run_setup_action(std::string_view action, const std::filesystem::path& install,
                     const SetupSession&);

// What the program supplies. An empty setup_action or run_transaction makes that command line
// unknown (they exist on Windows only); the others are skipped when empty.
struct UpdaterActions {
    std::function<int(std::string_view action, const std::filesystem::path& install)> setup_action;
    // Applies (recover false) or recovers the transaction and returns its final state.
    // launcher_pid and launcher_time identify the application that started a recovery and is
    // waited for; zero when there is none.
    std::function<TransactionState(std::string_view transaction_id, bool recover,
                                   std::uint32_t launcher_pid, std::uint64_t launcher_time)>
        run_transaction;
    // Called before a transaction runs (transaction_runner.h: leave_launch_directory).
    std::function<void()> leave_launch_directory;
    // The last-resort error box of a program without a window (update::native_message_box).
    std::function<void(std::string_view)> message_box;
    // Standard output and standard error when empty.
    std::function<void(std::string_view)> out;
    std::function<void(std::string_view)> err;
    // updater.log (updater_log.h: updater_log_path); empty: no log.
    std::filesystem::path log_file;
};

// Runs one command line and returns the exit code: 0 done, 1 failed, 2 not a command line.
//   --setup-action prepare|finish|rollback --app-dir <install>
//   --apply --transaction-id <id>
//   --recover --transaction-id <id> [--wait-for-launcher <pid> <creation time>]
//   --write-manifest <tree> --version <version>
//   --pack-archive <tree> --version <version> --out <zip>
//   --verify-archive <zip> [--version <version>]
// A command line that is none of these prints the usage to standard error and shows no box: the
// program is also run by build scripts, which a modal box would hang. A failed transaction shows
// its message in the box (other exceptions as "The update failed: ..."), unless the previous
// version was started again and reports it itself (ReportedByApplication). Setup actions and
// the packaging commands never show a box. Every command line but --help and the packaging
// commands (run by build scripts, not on the user's computer) first opens `log_file`, as
// setup_updater_logging does before anything else; a log that cannot be opened changes nothing.
int run_updater(std::span<const std::string> args, const UpdaterActions&);

} // namespace fv::update_native
