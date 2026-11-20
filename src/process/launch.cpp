#include "process/launch.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <exception>
#include <format>
#include <mutex>
#include <semaphore>
#include <system_error>
#include <thread>
#include <typeinfo>

#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "process/detail/launch_internal.h"
#include "process/detail/text.h"
#include "process/process_env.h"
#include "pyport/text.h"

namespace fv {

namespace {

constexpr log::Logger logger("services.actions.launch");

using Clock = std::chrono::steady_clock;

struct BackendSlot {
    std::mutex mutex;
    std::shared_ptr<LaunchBackend> backend;
};

BackendSlot& backend_slot() {
    // Never destroyed: an explorer thread may still hold the backend at exit.
    static auto* slot = new BackendSlot();
    return *slot;
}

struct HandlerCache {
    std::mutex mutex;
    std::map<std::string, std::pair<Clock::time_point, std::optional<bool>>> entries;
};

HandlerCache& handler_cache() {
    static auto* cache = new HandlerCache();
    return *cache;
}

// Held while Explorer is asked. A semaphore, because the thread that asks releases it.
std::binary_semaphore& explorer_turn() {
    static auto* turn = new std::binary_semaphore(1);
    return *turn;
}

// What to install for a scheme a preset or an agent link uses.
std::optional<std::string_view> scheme_app(std::string_view scheme) {
    if (scheme == "claude") {
        return "the Claude app";
    }
    if (scheme == "claude-cli") {
        return "Claude Code";
    }
    if (scheme == "codex") {
        return "the ChatGPT app";
    }
    return std::nullopt;
}

bool is_ascii_letter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// [A-Za-z][A-Za-z0-9+.\-]*
bool valid_scheme(std::string_view scheme) {
    if (scheme.empty() || !is_ascii_letter(scheme.front())) {
        return false;
    }
    return std::all_of(scheme.begin() + 1, scheme.end(), [](char c) {
        return is_ascii_letter(c) || (c >= '0' && c <= '9') || c == '+' || c == '.' || c == '-';
    });
}

bool no_handler(std::uint32_t result) {
    return result == NO_ASSOCIATION || result == ASSOC_NOT_FOUND;
}

// Whether ShellExecute has a handler for `scheme`: a COM object or a command
// (launch.py _windows_handler).
std::optional<bool> windows_handler(LaunchBackend& backend, std::string_view scheme) {
    const auto delegate = backend.assoc_query(scheme, AssocString::delegate_execute);
    if (delegate.result == ASSOC_OK && !delegate.value.empty()) {
        return true;
    }
    const auto command = backend.assoc_query(scheme, AssocString::command);
    if (command.result != ASSOC_OK || command.value.empty()) {
        if (no_handler(command.result) && no_handler(delegate.result)) {
            return false;
        }
        return std::nullopt;
    }
    // The lookup checks the program: one an uninstalled app left behind opens nothing.
    return backend.assoc_query(scheme, AssocString::executable).result != CO_E_APPNOTFOUND_HR;
}

void windows_open(std::string_view uri) {
    if (process_detail::ask_explorer(uri, "")) {
        return;
    }
    // Explorer's desktop isn't there (Explorer is restarting, another shell runs): only then
    // does the link open from this process, and its app get this process's environment.
    if (!launch_backend()->start_here(uri, "")) {
        throw LaunchError("No app on this computer opens that link.");
    }
}

// Windows runs these through cmd.exe or PowerShell, which would parse the words.
constexpr std::array<std::string_view, 3> kScriptSuffixes{".cmd", ".bat", ".ps1"};

void windows_terminal(LaunchBackend& backend, const std::vector<std::string>& argv,
                      const std::filesystem::path& cwd,
                      const std::map<std::string, std::string>& env) {
    const auto program = path_from_utf8(argv.front());
    const auto name = path_to_utf8(program.filename());
    const auto suffix = py::lower(path_to_utf8(program.filename().extension()));
    if (std::find(kScriptSuffixes.begin(), kScriptSuffixes.end(), suffix) !=
        kScriptSuffixes.end()) {
        throw LaunchError(std::format(
            "{} is a script, and Windows would read your words as commands on the way in. "
            "Install the program's own .exe build, or open it through its app instead.",
            name));
    }
    if (suffix != ".exe") {
        throw LaunchError(
            std::format("{} isn't a program {} can open in a terminal.", name, identity().product));
    }
    if (py::len(windows_command_line(argv)) > WINDOWS_COMMAND_LINE_MAX) {
        throw LaunchError("That's too long to open in a terminal. Try a shorter request.");
    }
    ProcessSpec spec;
    spec.program = program;
    spec.args.assign(argv.begin() + 1, argv.end());
    spec.cwd = cwd;
    spec.env = env;
    spec.own_tree = false;
    spec.new_console = true;
    if (!backend.start_console(spec)) {
        throw LaunchError(std::format("{} couldn't be started in a terminal.", name));
    }
}

} // namespace

void set_launch_backend(std::shared_ptr<LaunchBackend> backend) {
    {
        auto& slot = backend_slot();
        std::lock_guard lock(slot.mutex);
        slot.backend = std::move(backend);
    }
    forget_handlers();
}

std::shared_ptr<LaunchBackend> launch_backend() {
    auto& slot = backend_slot();
    std::lock_guard lock(slot.mutex);
    if (!slot.backend) {
        slot.backend = process_detail::platform_launch_backend();
    }
    return slot.backend;
}

void open_uri(std::string_view uri) {
    if (uri.empty() || uri.find_first_of(std::string_view("\r\n\0", 3)) != std::string_view::npos) {
        throw LaunchError("That link can't be opened.");
    }
    if (py::len(uri) > LINK_MAX_CHARS) {
        throw LaunchError("That's too long to send in a link. Try fewer words.");
    }
    const auto scheme = py::lower(uri.substr(0, uri.find(':')));
    // Windows would offer to find an app in the Store for a scheme nothing handles.
    if (scheme != "http" && scheme != "https" && handler_available(scheme) == false) {
        const auto app = scheme_app(scheme);
        throw LaunchError(std::format("No app on this computer opens {}: links.{}", scheme,
                                      app ? std::format(" Install {}.", *app) : ""));
    }
    if (!launch_backend()->supports_launching()) {
        throw LaunchError("Opening links is not supported on this platform.");
    }
    windows_open(uri);
}

std::optional<bool> handler_available(std::string_view scheme) {
    const auto cleaned = py::rstrip(py::lower(py::strip(scheme)), ":");
    if (!valid_scheme(cleaned)) {
        return false;
    }
    const auto backend = launch_backend();
    auto& cache = handler_cache();
    {
        std::lock_guard lock(cache.mutex);
        const auto it = cache.entries.find(cleaned);
        if (it != cache.entries.end() &&
            std::chrono::duration<double>(backend->now() - it->second.first).count() <
                HANDLER_CACHE_S) {
            return it->second.second;
        }
    }
    std::optional<bool> found;
    try {
        found = windows_handler(*backend, cleaned);
    } catch (const std::exception& error) {
        // A lookup must never break an action.
        logger.warning("Handler lookup failed ({})", typeid(error).name());
        found = std::nullopt;
    }
    std::lock_guard lock(cache.mutex);
    cache.entries.insert_or_assign(cleaned, std::make_pair(backend->now(), found));
    return found;
}

void forget_handlers() {
    auto& cache = handler_cache();
    std::lock_guard lock(cache.mutex);
    cache.entries.clear();
}

void open_in_terminal(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
                      const std::optional<std::map<std::string, std::string>>& env) {
    if (argv.empty() || argv.front().empty()) {
        throw LaunchError("There's no program to open.");
    }
    if (std::any_of(argv.begin(), argv.end(),
                    [](const std::string& arg) { return arg.find('\0') != std::string::npos; })) {
        throw LaunchError("Those words can't be passed to a program.");
    }
    const auto workdir = cwd.empty() ? path_from_utf8(process_detail::home_folder()) : cwd;
    std::error_code error;
    if (workdir.empty() || !std::filesystem::is_directory(workdir, error)) {
        throw LaunchError("The working folder doesn't exist.");
    }
    const auto child = env ? *env : to_spec_env(child_env());
    const auto backend = launch_backend();
    if (!backend->supports_launching()) {
        throw LaunchError("Opening a terminal is not supported on this platform.");
    }
    windows_terminal(*backend, argv, workdir, child);
}

namespace process_detail {

bool ask_explorer(std::string_view file, std::string_view args) {
    auto backend = launch_backend();
    const double wait_s = backend->explorer_wait_s();
    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                             std::chrono::duration<double>(std::max(0.0, wait_s)));
    if (!explorer_turn().try_acquire_until(deadline)) {
        throw NoAnswer(std::string(NO_ANSWER_MESSAGE));
    }
    struct Outcome {
        std::mutex mutex;
        std::condition_variable ready;
        bool done = false;
        bool opened = false;
        std::exception_ptr error;
    };
    auto outcome = std::make_shared<Outcome>();
    try {
        std::thread([outcome, backend, file = std::string(file), args = std::string(args)] {
            bool opened = false;
            std::exception_ptr error;
            {
                ComScope com;
                try {
                    opened = backend->explorer_open(file, args);
                } catch (...) {
                    error = std::current_exception();
                }
            }
            {
                std::lock_guard lock(outcome->mutex);
                outcome->opened = opened;
                outcome->error = error;
                outcome->done = true;
            }
            outcome->ready.notify_all();
            explorer_turn().release();
        }).detach();
    } catch (...) {
        explorer_turn().release();
        throw;
    }
    std::unique_lock lock(outcome->mutex);
    if (!outcome->ready.wait_until(lock, deadline, [&] { return outcome->done; })) {
        logger.warning("Explorer didn't take a link within {:.0f} s", wait_s);
        throw NoAnswer(std::string(NO_ANSWER_MESSAGE));
    }
    if (outcome->error) {
        std::rethrow_exception(outcome->error);
    }
    return outcome->opened;
}

} // namespace process_detail

} // namespace fv
