#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "process/launcher.h"

// Opening links and terminals for actions (services/actions/launch.py). Words reach a link
// percent-encoded and a terminal as one argv element; they never pass through a shell or
// cmd.exe. On Windows a link opens through Explorer, the user's shell, so its app starts with
// Explorer's environment rather than this process's. macOS and Linux have no implementation:
// handler_available can't tell there, and open_uri and open_in_terminal refuse.
namespace fv {

// How long a handler lookup is trusted, in seconds; apps are rarely installed mid-session.
inline constexpr double HANDLER_CACHE_S = 60.0;
// Windows rejects longer command lines (32,767 characters with the program).
inline constexpr std::size_t WINDOWS_COMMAND_LINE_MAX = 32'000;
// A link travels on its handler's command line, so longer ones (in code points) can't open.
inline constexpr std::size_t LINK_MAX_CHARS = 30'000;
// Claude Desktop cuts a claude:// link's q near 14,000 characters.
inline constexpr std::size_t CLAUDE_APP_TEXT_MAX = 14'000;
// How long Explorer may take to take a link, in seconds. ShellExecute returns once the link's
// app has started or a rundll32 or DDE handler has the link, and may wait while an elevation
// prompt is up; this still gets the action's worker back from an Explorer whose desktop hung.
inline constexpr double EXPLORER_WAIT_S = 30.0;

inline constexpr std::string_view NO_ANSWER_MESSAGE = "Windows didn't answer. Try again.";

// Opening failed; the message is safe to show.
class LaunchError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Windows didn't answer in time. It may still open the link, so nothing else should.
class NoAnswer : public LaunchError {
public:
    using LaunchError::LaunchError;
};

// What AssocQueryStringW is asked for (ASSOCSTR_*).
enum class AssocString : int {
    command = 1,
    executable = 2,
    delegate_execute = 18, // the COM object of a packaged app's or Windows' own protocol
};

// HRESULTs of a lookup, as unsigned.
inline constexpr std::uint32_t ASSOC_OK = 0;
inline constexpr std::uint32_t NO_ASSOCIATION = 0x80070483;      // HRESULT(ERROR_NO_ASSOCIATION)
inline constexpr std::uint32_t ASSOC_NOT_FOUND = 0x80070490;     // HRESULT(ERROR_NOT_FOUND)
inline constexpr std::uint32_t CO_E_APPNOTFOUND_HR = 0x800401F5; // the handler's program is gone

struct AssocAnswer {
    std::uint32_t result = NO_ASSOCIATION;
    std::string value;
};

// The platform calls under this file, replaced in tests (process/testing/fake_launch_backend.h).
// Affinity: thread-safe.
class LaunchBackend {
public:
    virtual ~LaunchBackend() = default;
    // False where links and terminals can't be opened at all (not Windows).
    virtual bool supports_launching() const { return true; }
    // AssocQueryStringW for a protocol, with COM on the calling thread while it asks.
    virtual AssocAnswer assoc_query(std::string_view scheme, AssocString what) = 0;
    // ShellExecute of `file` with `args` and the default verb, run inside Explorer through its
    // desktop's Shell.Application. False when Explorer's desktop can't be asked. Called on a
    // thread of its own that has a single-threaded COM apartment.
    virtual bool explorer_open(std::string_view file, std::string_view args) = 0;
    // ShellExecute in this process (os.startfile), with COM; false when nothing opened it.
    virtual bool start_here(std::string_view file, std::string_view args) = 0;
    // Starts a terminal program (spec.new_console); false when it couldn't be started.
    virtual bool start_console(const ProcessSpec& spec) = 0;
    // Opens the folder holding `path` with it selected.
    virtual bool reveal(const std::filesystem::path& path) = 0;
    // The monotonic time the handler cache reads.
    virtual std::chrono::steady_clock::time_point now() const {
        return std::chrono::steady_clock::now();
    }
    // How long Explorer is waited for, in seconds (EXPLORER_WAIT_S).
    virtual double explorer_wait_s() const { return EXPLORER_WAIT_S; }
};

// Replaces the platform backend (nullptr: back to the real one) and forgets cached handlers.
void set_launch_backend(std::shared_ptr<LaunchBackend> backend);
std::shared_ptr<LaunchBackend> launch_backend();

// Hands `uri` to the app registered for its scheme; blocks briefly. Throws NoAnswer when Windows
// didn't answer in time (it may still open the link, so don't open it, or another, some other
// way), LaunchError when nothing could open it or it is too long to; the message names the app
// to install when one is known.
void open_uri(std::string_view uri);

// Whether an app is registered for `scheme`; nullopt when it can't be told. Blocking (a registry
// lookup); answers are cached for HANDLER_CACHE_S. Never throws.
std::optional<bool> handler_available(std::string_view scheme);

// Drops cached handler lookups, for example after the user installs an app.
void forget_handlers();

// Runs `argv` in a new terminal window; returns once the window is asked for. Windows starts
// the .exe directly in a new console, so every element reaches the program as one argument.
// `env` defaults to child_env(); `cwd` to the home folder. Throws LaunchError saying why no
// terminal could be opened.
void open_in_terminal(const std::vector<std::string>& argv, const std::filesystem::path& cwd = {},
                      const std::optional<std::map<std::string, std::string>>& env = std::nullopt);

} // namespace fv
