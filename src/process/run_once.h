#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"
#include "process/launcher.h"

// Running a program once without a shell or a window (services/automation/programs.py run_once):
// the engine under program hooks, program filters and program actions.
namespace fv {

// Most of a program's stdout that is kept; the rest is read and discarded.
inline constexpr std::size_t MAX_STDOUT_BYTES = 1 << 20;
// How long, after a program exits by itself, run_once keeps reading output and writing input
// through pipes that processes it left running hold open, in seconds.
inline constexpr double OUTPUT_GRACE_S = 1.0;
// The same after a run was stopped (its whole tree was killed), in seconds.
inline constexpr double STOPPED_GRACE_S = 2.0;
// How often a run checks its cancel token, in seconds.
inline constexpr double CANCEL_POLL_S = 0.05;
// The shortest time limit, in seconds.
inline constexpr double MIN_TIMEOUT_S = 0.05;

inline constexpr std::string_view CANCELED = "Canceled";
inline constexpr std::string_view CHOOSE_PROGRAM = "Choose the program to run.";
// For callers that read a time limit from text (Python: float() failed).
inline constexpr std::string_view TIME_LIMIT_NOT_A_NUMBER = "The time limit isn't a number.";
inline constexpr std::string_view WORKING_FOLDER_MISSING = "The working folder doesn't exist.";
inline constexpr std::string_view ARGUMENTS_NOT_VALID =
    "The program couldn't start (its arguments aren't valid).";
inline constexpr std::string_view PROGRAM_NOT_FOUND = "The program wasn't found.";
inline constexpr std::string_view PROGRAM_NOT_ALLOWED =
    "The program isn't allowed to run (permission denied).";
inline constexpr std::string_view PROGRAM_NOT_RUNNABLE =
    "Windows can't run that file directly. Choose a program (.exe).";

struct RunOnceResult {
    // The program's exit code when it ran to its end (whatever the code); nullopt otherwise.
    std::optional<int> exit_code;
    std::string output; // stdout, at most MAX_STDOUT_BYTES
    // "" when the program ran to its end, else why it didn't: "Timed out after N s", CANCELED,
    // or why it couldn't start.
    std::string error;
    bool timed_out = false;
    bool canceled = false;
    double elapsed_s = 0.0;
};

// Runs `argv` once and never throws. `input` is written to stdin by a thread of its own, which
// then closes it; stderr is discarded. The program runs in its own process group and job, in
// `cwd` (empty: the home folder), with child_env(extra_env). When `timeout_s` (at least
// MIN_TIMEOUT_S) passes or `cancel` fires (polled every CANCEL_POLL_S), the program and every
// process it started are stopped. A program that ends by itself leaves what it started running
// (an app it opened stays open); for OUTPUT_GRACE_S more at most its output is read and what is
// left of `input` written, then both pipes are closed. Blocks the calling thread.
RunOnceResult run_once(const std::vector<std::string>& argv, std::string_view input,
                       double timeout_s, const CancelToken& cancel = {},
                       std::filesystem::path cwd = {},
                       std::map<std::string, std::string> extra_env = {},
                       ProcessLauncher* launcher = nullptr);

// "Timed out after {round(timeout, 3):g} s".
std::string timed_out_message(double timeout_s);

} // namespace fv
