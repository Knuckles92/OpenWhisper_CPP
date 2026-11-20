// fiorivoce.exe. The early command line (--help, --version, --diagnostics, --self-test-gui)
// answers before any Qt application, lease or speech runtime exists; anything else passes the
// update launch gate and opens the app (ui_shell/bootstrap.h). An app that exits with
// backup::restart_exit_code (a restore or import was staged, or the code-change offer) is started
// again here, once it has released everything. --api and --channel are fiorivoce-cli's: they are
// forwarded to it before anything else happens.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "appkit/app_context.h"
#include "appkit/feature.h"
#include "backup/restart.h"
#include "base/clock.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "engine/platform.h"
#include "http/version.h"
#include "process/detached.h"
#include "ui_shell/bootstrap.h"
#include "ui_shell/early_cli.h"
#include "ui_shell/restart_tail.h"
#include "update/code_identity.h"
#include "update/gates.h"
#include "update/install_probe.h"
#include "update/release_identity.h"
#include "update_native/health.h"
#include "update_native/installed_transaction.h"
#include "update_native/startup_recovery.h"
#include "update_native/win_registry_actions.h"
#include <algorithm>
#include <functional>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

// The running executable: what the restart starts again and what the code-change offer watches.
std::filesystem::path executablePath() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error);
#endif
}

// The restart after a restore, started detached with no console (ui_shell/restart_tail.h).
class DetachedStarter final : public fv::backup::ProcessStarter {
public:
    std::expected<void, std::string> start(const std::filesystem::path& exe,
                                           std::span<const std::string> args) override {
        const auto started = fv::start_detached(exe, args);
        if (!started) {
            return std::unexpected(started.error());
        }
        return {};
    }
};

// Hands the app mutexes main() holds to the update hand-off (AppContext::find<LaunchGate>), so the
// setup can be started with them released.
class LaunchGateFeature final : public fv::Feature {
public:
    explicit LaunchGateFeature(fv::update::LaunchGate& gate) : gate_(gate) {}
    std::string_view id() const override { return "launch_gate"; }
    void provide(fv::AppContext& context) override {
        // Not owned: main() keeps the gate until the process ends.
        context.provide(
            std::shared_ptr<fv::update::LaunchGate>(&gate_, [](fv::update::LaunchGate*) {}));
    }
    void install(fv::AppContext&) override {}

private:
    fv::update::LaunchGate& gate_;
};

// fiorivoce.exe --api and --channel run fiorivoce-cli from this executable's folder with the same
// arguments and this process's standard handles, and end with its exit code, so a configuration
// that names this executable keeps working. Nothing here creates a window, a lease or a Qt object.
#if defined(_WIN32)
bool usableHandle(HANDLE handle) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    SetLastError(NO_ERROR);
    return GetFileType(handle) != FILE_TYPE_UNKNOWN || GetLastError() == NO_ERROR;
}

bool redirectedHandle(HANDLE handle) {
    if (!usableHandle(handle)) {
        return false;
    }
    const DWORD type = GetFileType(handle);
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

HANDLE inheritableCopy(HANDLE handle) {
    HANDLE copy = nullptr;
    if (!usableHandle(handle) || !DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
                                                  &copy, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        return nullptr;
    }
    return copy;
}

HANDLE openConsole(const wchar_t* name, DWORD access) {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const HANDLE handle = CreateFileW(name, access, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                      OPEN_EXISTING, 0, nullptr);
    return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
}

// The forwarder waits for the CLI's exit code; a Ctrl+C in a shared console is the CLI's to
// handle. (A handler function, unlike SetConsoleCtrlHandler(nullptr, TRUE), is not inherited.)
BOOL WINAPI keepWaiting(DWORD) {
    return TRUE;
}

int forwardToCli(const std::filesystem::path& exe, std::span<const std::string> options) {
    const bool channel = options.front() == "--channel";
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    // A windowed start without pipes (a double-clicked shortcut): nothing to talk to, no dialog.
    // With more arguments the CLI answers first (help 0, anything else 2), as main.py's did.
    if (channel && options.size() == 1 && (!usableHandle(in) || !usableHandle(out))) {
        return 2;
    }
    // --api started from a terminal shares that console, so its output shows there and Ctrl+C
    // reaches it; what was redirected stays redirected.
    bool console = false;
    std::array<HANDLE, 3> handles{};
    if (!channel && !redirectedHandle(out) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        console = true;
        SetConsoleCtrlHandler(keepWaiting, TRUE);
        handles[0] = redirectedHandle(in) ? inheritableCopy(in)
                                          : openConsole(L"CONIN$", GENERIC_READ | GENERIC_WRITE);
        handles[1] = redirectedHandle(out) ? inheritableCopy(out)
                                           : openConsole(L"CONOUT$", GENERIC_READ | GENERIC_WRITE);
        handles[2] = redirectedHandle(err) ? inheritableCopy(err)
                                           : openConsole(L"CONOUT$", GENERIC_READ | GENERIC_WRITE);
    } else {
        handles = {inheritableCopy(in), inheritableCopy(out), inheritableCopy(err)};
    }
    std::vector<HANDLE> inherited;
    for (HANDLE handle : handles) {
        if (handle != nullptr) {
            inherited.push_back(handle);
        }
    }
    const auto closeHandles = [&] {
        for (HANDLE handle : inherited) {
            CloseHandle(handle);
        }
    };

    const std::filesystem::path cli = exe.parent_path() / L"fiorivoce-cli.exe";
    // The program is passed apart, so its quoted name only has to read back as argv[0].
    std::vector<std::string> argv{"fiorivoce-cli.exe"};
    argv.insert(argv.end(), options.begin(), options.end());
    const std::string line = fv::list2cmdline(argv);
    std::wstring command(static_cast<std::size_t>(MultiByteToWideChar(
                             CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0)),
                         L'\0');
    MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), command.data(),
                        static_cast<int>(command.size()));

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = handles[0];
    startup.StartupInfo.hStdOutput = handles[1];
    startup.StartupInfo.hStdError = handles[2];
    // Only these handles reach the child.
    std::vector<char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | (console ? 0 : CREATE_NO_WINDOW);
    if (!inherited.empty()) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage.resize(size);
        attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (InitializeProcThreadAttributeList(attributes, 1, 0, &size) &&
            UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      inherited.data(), inherited.size() * sizeof(HANDLE), nullptr,
                                      nullptr)) {
            startup.lpAttributeList = attributes;
            flags |= EXTENDED_STARTUPINFO_PRESENT;
        }
    }
    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(cli.c_str(), command.data(), nullptr, nullptr,
                                        startup.lpAttributeList != nullptr ? TRUE : FALSE, flags,
                                        nullptr, nullptr, &startup.StartupInfo, &info);
    if (attributes != nullptr) {
        DeleteProcThreadAttributeList(attributes);
    }
    closeHandles();
    if (!created) {
        const std::string message = "Could not start fiorivoce-cli.exe.\n";
        DWORD written = 0;
        if (usableHandle(err)) {
            WriteFile(err, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
        }
        return 1;
    }
    // Ending this process (Claude Code stopping the server) ends the CLI with it.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job, info.hProcess);
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    if (job != nullptr) {
        CloseHandle(job);
    }
    return static_cast<int>(code);
}
#else
int forwardToCli(const std::filesystem::path& exe, std::span<const std::string> options) {
    const std::filesystem::path cli = exe.parent_path() / "fiorivoce-cli";
    std::vector<std::string> argv{cli.string()};
    argv.insert(argv.end(), options.begin(), options.end());
    std::vector<char*> raw;
    for (std::string& arg : argv) {
        raw.push_back(arg.data());
    }
    raw.push_back(nullptr);
    const pid_t child = fork();
    if (child == 0) {
        execv(cli.c_str(), raw.data());
        _exit(127);
    }
    if (child < 0) {
        std::fprintf(stderr, "Could not start %s.\n", cli.filename().c_str());
        return 1;
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
#endif

[[noreturn]] void leave(int code) {
    std::fflush(stdout);
    std::fflush(stderr);
    // main.py's os._exit: every cleanup ran in Bootstrap, and no static destructor may touch a
    // speech runtime that is never unloaded.
    std::_Exit(code);
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = fv::utf8_args(argc, argv);
    const std::span<const std::string> options =
        args.empty() ? std::span<const std::string>() : std::span(args).subspan(1);

    // main.py's --api and --channel, which fiorivoce-cli runs: before any window, tray icon,
    // single-instance check or launch gate.
    if (!options.empty() && (options.front() == "--api" || options.front() == "--channel")) {
        leave(forwardToCli(executablePath(), options));
    }

    // The workflow smoke lives in fiorivoce-cli (Qt-free); this executable refuses it with its
    // usage line.
    if (!options.empty() && options.front() == "--workflow-smoke") {
        fv::attachParentConsole();
        const fv::AppIdentity& id = fv::identity();
        std::string executable(id.product);
        for (char& c : executable) {
            c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
        const std::string usage = fv::guiUsageText(executable, id.product, id.env_prefix);
        std::fputs(usage.substr(0, usage.find('\n') + 1).c_str(), stderr);
        leave(2);
    }

    const fv::EarlyCli cli = fv::parseEarlyCli(args);
    const bool opensApp = cli.command == fv::EarlyCommand::Gui;
    std::optional<fv::update::LaunchGate> gate;
    std::function<bool()> acknowledgeUpdate;
    std::filesystem::path exe;
    if (opensApp) {
        if (!fv::update::skips_launch_gate(options)) {
            // main.py's _early_update_gate: before any Qt object exists.
            const fv::update::ReleaseIdentity& release = fv::update::release_identity();
            const auto registration = fv::update::discover_install_registration(release);
            const auto probe =
                fv::update::make_install_probe(registration, fv::update::running_app_dir());
            try {
                if (const auto health = fv::update_native::health_request(options)) {
                    if (!registration ||
                        fv::update::detect_channel(probe) != fv::update::InstallChannel::Installer)
                        leave(1);
                    const auto paths = fv::Paths::resolve(fv::identity(), fv::system_env());
                    const auto executable = executablePath();
                    fv::update_native::JournalContext context{
                        paths.data_root(), registration->install_location,
                        release.app_id,    executable,
                        registration,      R"(Software\Microsoft\Windows\CurrentVersion\RunOnce)"};
                    if (!fv::update_native::check_health(context, *health, executable,
                                                         fv::http::kAppVersion, false))
                        leave(1);
                    acknowledgeUpdate = [context, request = *health, executable] {
                        const auto names = fv::update::release_identity().update_mutexes();
                        auto& api = fv::update::system_mutex_api();
                        return std::ranges::any_of(
                                   names, [&api](const auto& name) { return api.exists(name); }) &&
                               fv::update_native::check_health(context, request, executable,
                                                               fv::http::kAppVersion, true);
                    };
                }
            } catch (const std::exception&) {
                leave(1);
            }
#if defined(_WIN32)
            if (!acknowledgeUpdate && !probe.all_users &&
                fv::update_native::standard_user_process() &&
                fv::update::detect_channel(probe) == fv::update::InstallChannel::Installer) {
                const auto paths = fv::Paths::resolve(fv::identity(), fv::system_env());
                auto& mutex = fv::update::system_mutex_api();
                const auto held = [&mutex](const auto& name) {
                    return mutex.exists(name);
                };
                if (paths.data_root() == paths.local_app_dir() && !held(release.setup_mutex()) &&
                    !std::ranges::any_of(release.update_mutexes(), held) &&
                    !std::ranges::any_of(release.app_mutexes(), held)) {
                    try {
                        const auto context = fv::update_native::registered_context(paths);
                        if (std::filesystem::equivalent(context.parent_exe, executablePath()) &&
                            fv::update_native::recover_before_start(
                                context, mutex, GetCurrentProcessId(),
                                fv::update_native::current_process_creation_time(),
                                [](const auto& helper, const auto& arguments) {
                                    return fv::start_detached(helper, arguments).has_value();
                                },
                                [&context](const auto& journal) {
                                    fv::update_native::WinRegistryActions registry(
                                        context, journal.transaction_id);
                                    registry.clear_recovery(journal);
                                    return true;
                                }))
                            leave(0);
                    } catch (const std::exception& error) {
                        fv::update::native_message_box(error.what());
                        leave(1);
                    }
                }
            }
#endif
            auto entered = fv::update::LaunchGate::enter(
                release, probe, fv::update::system_mutex_api(), fv::system_clock(),
                static_cast<bool>(acknowledgeUpdate));
            if (!entered) {
                if (acknowledgeUpdate)
                    leave(1);
                if (!entered.error().empty()) {
                    fv::update::native_message_box(entered.error());
                }
                leave(0);
            }
            gate.emplace(std::move(*entered));
        }
        exe = executablePath();
        // What a later change on disk is measured against (update/code_identity.h).
        fv::update::CodeIdentity::remember_launch(exe);
    }

    fv::BootstrapHooks hooks;
    hooks.arguments = args;
    hooks.acknowledgeUpdate = std::move(acknowledgeUpdate);
    hooks.features = [&gate] {
        auto features = fv::FeatureRegistry::createAll();
        if (gate) {
            features.push_back(std::make_unique<LaunchGateFeature>(*gate));
        }
        return features;
    };
    const int code = fv::appMain(argc, argv, std::move(hooks));

    // A restart exit code starts the app again (ui_shell/restart_tail.h).
    DetachedStarter starter;
    leave(fv::finishRun(
        code, opensApp,
        [&gate] {
            if (gate) {
                gate->release_for_setup();
            }
        },
        exe, starter, fv::withDefaults({}).errorBox));
}
