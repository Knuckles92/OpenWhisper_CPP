#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fv {

// Receives a child's output one line at a time, without the line ending, decoded as UTF-8 with
// invalid bytes replaced. Runs on the pipe's reader thread "fv-pipe-<pid>-out" or "-err".
using LineSink = std::function<void(std::string_view line)>;

// Receives a child's output as it arrives, on the pipe's reader thread. After the last chunk it
// is called once more with an empty span: the pipe is done (end of file, a broken pipe, or
// release_pipes).
using ByteSink = std::function<void(std::span<const char>)>;

struct ProcessSpec {
    std::filesystem::path program;
    std::vector<std::string> args; // UTF-8, quoted by the launcher
    std::filesystem::path cwd;     // empty: this process's working folder
    // Empty: child_env() of process/process_env.h (the user's environment without this app's
    // .env keys and internal tokens).
    std::map<std::string, std::string> env;
    bool own_tree = true; // suspended start, job assigned, then resumed
    bool pipes = false;   // stdin, stdout and stderr are pipes
    bool disown_on_clean_exit = false;
    bool stdin_null = false;        // stdin from NUL (probes)
    bool stderr_null = false;       // stderr to NUL (run_once discards it)
    bool new_console = false;       // CREATE_NEW_CONSOLE, no job, no pipes (terminals)
    bool new_process_group = false; // CREATE_NEW_PROCESS_GROUP
};

// A started child. Every process is started without a console window (CREATE_NO_WINDOW)
// unless the spec asks for a new console. With own_tree it and everything it starts are in a
// job that ends them all when the job is closed; destroying the Process closes it (after a clean
// exit with disown_on_clean_exit, it disowns instead).
// Affinity: thread-safe.
class Process {
public:
    virtual ~Process() = default;
    virtual std::uint32_t pid() const = 0;
    // Queued for the stdin writer thread "fv-pipe-<pid>-in", so a child that never reads cannot
    // block the caller. Ignored without a stdin pipe.
    virtual void write_stdin(std::string_view) = 0;
    virtual void on_line(LineSink stdout_sink, LineSink stderr_sink) = 0; // reader threads
    virtual std::optional<int> wait(std::chrono::milliseconds) = 0;
    // Ends the process and everything in its job; without a job (or after disown) the process
    // and its descendants by parent id, as taskkill /T /F.
    virtual void kill_tree() = 0;

    // The raw alternative to on_line; one of the two, once. A null sink reads and drops.
    virtual void on_bytes(ByteSink stdout_sink, ByteSink stderr_sink) = 0;
    // Closes stdin once what write_stdin queued is written.
    virtual void close_stdin() = 0;
    virtual std::optional<int> exit_code() const = 0; // nullopt while running
    // Stops owning what the process started: the job's kill-on-close limit is cleared and the
    // job closed, so an app the program opened stays open (programs.py _disown).
    virtual void disown() = 0;
    // Gives the pipe threads until `grace` to finish (end of file, or stdin written), then
    // cancels the read or write each is blocked in until it leaves, and closes the pipes. A
    // helper the child left running may hold an inherited pipe end forever; its later writes
    // fail and what is left of stdin is dropped.
    virtual void release_pipes(std::chrono::milliseconds grace) = 0;
};

// Windows: CreateProcessW with CREATE_SUSPENDED | CREATE_NO_WINDOW, job assignment before resume.
// Only the three standard handles are inherited. A failure to start is the sentence run_once
// shows ("The program wasn't found.", see process/run_once.h).
// Affinity: thread-safe.
class ProcessLauncher {
public:
    virtual ~ProcessLauncher() = default;
    virtual std::expected<std::unique_ptr<Process>, std::string> start(const ProcessSpec&) = 0;
};

// Affinity: thread-safe.
class Shell {
public:
    virtual ~Shell() = default;
    virtual bool open_url(std::string_view) = 0;
    virtual bool reveal(const std::filesystem::path&) = 0;
    virtual bool launch_via_explorer(const std::filesystem::path&, std::string_view args) = 0;
};

// The real launcher; elsewhere than Windows every start fails ("not supported on this
// platform").
std::shared_ptr<ProcessLauncher> make_process_launcher();
// False where that launcher's every start fails, so callers and tests can tell before trying.
bool can_start_programs();
// open_url is open_uri (process/launch.h), reveal selects the file in a folder window, and
// launch_via_explorer runs a path through Explorer's ShellExecute (in this process when Explorer's
// desktop can't be asked). Each returns false instead of throwing.
std::shared_ptr<Shell> make_shell();
// subprocess.list2cmdline: one Windows command line from an argument list.
std::string windows_command_line(const std::vector<std::string>& argv);

} // namespace fv
