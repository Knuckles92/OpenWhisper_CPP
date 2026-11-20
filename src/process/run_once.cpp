#include "process/run_once.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <system_error>
#include <utility>

#include "base/utf8.h"
#include "process/agent_process.h"
#include "process/detail/text.h"
#include "process/process_env.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv {

namespace {

using Clock = std::chrono::steady_clock;

Clock::duration seconds(double s) {
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s));
}

// The home folder, when it is one (programs.py _default_cwd).
std::filesystem::path default_cwd() {
    const auto home = process_detail::home_folder();
    if (home.empty()) {
        return {};
    }
    std::error_code error;
    auto path = path_from_utf8(home);
    return std::filesystem::is_directory(path, error) ? path : std::filesystem::path{};
}

} // namespace

std::string timed_out_message(double timeout_s) {
    double shown = timeout_s;
    if (std::isfinite(timeout_s)) {
        try {
            shown = py::round_ndigits(timeout_s, 3);
        } catch (const std::exception&) {
            shown = timeout_s;
        }
    }
    return std::format("Timed out after {:g} s", shown);
}

RunOnceResult run_once(const std::vector<std::string>& argv, std::string_view input,
                       double timeout_s, const CancelToken& cancel, std::filesystem::path cwd,
                       std::map<std::string, std::string> extra_env, ProcessLauncher* launcher) {
    const auto started = Clock::now();
    RunOnceResult result;
    const auto finish = [&](RunOnceResult r) {
        r.elapsed_s = std::chrono::duration<double>(Clock::now() - started).count();
        return r;
    };
    if (argv.empty() || py::strip(argv.front()).empty()) {
        result.error = CHOOSE_PROGRAM;
        return finish(std::move(result));
    }
    // max(0.05, nan) is 0.05 in Python.
    const double timeout =
        std::isnan(timeout_s) ? MIN_TIMEOUT_S : std::max(MIN_TIMEOUT_S, timeout_s);
    std::error_code error;
    if (!cwd.empty() && !std::filesystem::is_directory(cwd, error)) {
        result.error = WORKING_FOLDER_MISSING;
        return finish(std::move(result));
    }
    if (cancel.cancelled()) {
        result.error = CANCELED;
        result.canceled = true;
        return finish(std::move(result));
    }

    ProcessSpec spec;
    spec.program = path_from_utf8(argv.front());
    spec.args.assign(argv.begin() + 1, argv.end());
    spec.cwd = cwd.empty() ? default_cwd() : cwd;
    spec.env = to_spec_env(child_env(extra_env));
    spec.own_tree = true;
    spec.pipes = true;
    spec.stderr_null = true;
    spec.new_process_group = true;

    std::shared_ptr<ProcessLauncher> owned;
    if (launcher == nullptr) {
        owned = make_process_launcher();
        launcher = owned.get();
    }
    auto started_process = launcher->start(spec);
    if (!started_process) {
        result.error = started_process.error();
        return finish(std::move(result));
    }
    auto& process = **started_process;

    auto kept = std::make_shared<std::string>();
    auto kept_mutex = std::make_shared<std::mutex>();
    process.on_bytes(
        [kept, kept_mutex](std::span<const char> bytes) {
            std::lock_guard lock(*kept_mutex);
            if (kept->size() < MAX_STDOUT_BYTES) {
                kept->append(bytes.data(), std::min(bytes.size(), MAX_STDOUT_BYTES - kept->size()));
            }
        },
        nullptr);
    process.write_stdin(input);
    process.close_stdin();

    // programs.py _wait: "" once it exits, else why it must be stopped.
    // Past about 30 years the clock's nanoseconds would overflow; Python's float deadline never
    // comes either.
    const bool forever = timeout >= 1e9;
    const auto deadline = forever ? Clock::time_point::max() : Clock::now() + seconds(timeout);
    std::optional<int> code;
    for (;;) {
        const auto now = Clock::now();
        if (!forever && now >= deadline) {
            result.error = timed_out_message(timeout);
            result.timed_out = true;
            break;
        }
        const auto step = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::min(forever ? seconds(CANCEL_POLL_S) : deadline - now, seconds(CANCEL_POLL_S)));
        code = process.wait(std::max(step, std::chrono::milliseconds(1)));
        if (code) {
            break;
        }
        if (cancel.cancelled()) {
            result.error = CANCELED;
            result.canceled = true;
            break;
        }
    }

    Clock::duration grace;
    if (!code) {
        // Stops the whole tree, which also closes the pipes it held.
        kill_process_tree(process);
        grace = seconds(STOPPED_GRACE_S);
    } else {
        // It ended by itself: what it started is the user's now.
        process.disown();
        grace = seconds(OUTPUT_GRACE_S);
        result.exit_code = code;
    }
    process.release_pipes(std::chrono::duration_cast<std::chrono::milliseconds>(grace));
    {
        std::lock_guard lock(*kept_mutex);
        result.output = *kept;
    }
    started_process->reset();
    return finish(std::move(result));
}

} // namespace fv
