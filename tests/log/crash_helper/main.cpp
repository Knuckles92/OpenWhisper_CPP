// fv_log_crash_helper <mode> <data folder>
//
//   access-violation  installs the crash handler and writes through a null pointer
//   terminate         installs the crash handler and calls std::terminate
//   watcher-gone      installs the crash handler, ends fv-crash-watch as ExitProcess would, and
//                     writes through a null pointer (Windows)
//   logging           runs install_app_logging, logs, records a metric and exits normally
//   logging-stalled   runs install_app_logging, stalls fv-log, logs, and exits 0 when releasing
//                     the logging took at most 1.5 s (4 when it took longer)

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "base/identity.h"
#include "base/log.h"
#include "base/metrics.h"
#include "base/paths.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "log/crash_handler.h"
#include "log/setup.h"
#include "log/writer.h"

#if defined(_WIN32)
#include <windows.h>

#include <tlhelp32.h>
#endif

namespace {

constexpr fv::log::Logger kTest{"fv.test"};

// Read through volatile, so the compiler cannot see that the write goes to address zero.
int* volatile g_target = nullptr;

#if defined(_WIN32)
// Ends this process's fv-crash-watch thread; false when it was not found.
bool end_crash_watcher() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ended = false;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more && !ended;
         more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId()) {
            continue;
        }
        const HANDLE thread =
            OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_TERMINATE | SYNCHRONIZE, FALSE,
                       entry.th32ThreadID);
        if (!thread) {
            continue;
        }
        PWSTR name = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &name)) && name &&
            std::wstring_view(name) == L"fv-crash-watch") {
            ended =
                TerminateThread(thread, 0) && WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0;
        }
        LocalFree(name);
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return ended;
}
#endif

// Holds fv-log inside a job until released.
struct Stall {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Whatever happens, no error dialog reaches the screen.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    if (argc != 3) {
        std::fputs("usage: fv_log_crash_helper <mode> <data folder>\n", stderr);
        return 2;
    }
    const std::string_view mode = argv[1];
    fv::MapEnvReader env;
    env.set(std::string(fv::identity().env_prefix) + "DATA_DIR", argv[2]);
    fv::pin_per_user_folders(env, fv::path_from_utf8(argv[2]) / "per-user");
    const auto paths = fv::Paths::resolve(fv::identity(), env);

    if (mode == "logging") {
        auto logging = fv::log::install_app_logging(paths, fv::identity(), env, false);
        kTest.info("message");
        kTest.error_with(std::system_error(std::make_error_code(std::errc::timed_out)),
                         "private failure text");
        // setup_logging puts httpx and httpcore at WARNING: their INFO lines carry full URLs.
        constexpr fv::log::Logger httpx{"httpx"};
        constexpr fv::log::Logger httpcore{"httpcore.http11"};
        httpx.info("GET https://example.invalid/?q=dictated words");
        httpcore.info("send_request_headers.started");
        httpcore.warning("connection reset");
        fv::metrics::record("shutdown_s", 0.25);
        return 0;
    }
    if (mode == "logging-stalled") {
        auto stall = std::make_shared<Stall>();
        std::chrono::steady_clock::duration took{};
        {
            auto logging = fv::log::install_app_logging(paths, fv::identity(), env, false);
            logging.writer()->post([stall] {
                std::unique_lock lock(stall->mutex);
                stall->entered = true;
                stall->changed.notify_all();
                stall->changed.wait(lock, [&] { return stall->released; });
            });
            {
                std::unique_lock lock(stall->mutex);
                if (!stall->changed.wait_for(lock, std::chrono::seconds(10),
                                             [&] { return stall->entered; })) {
                    return 5;
                }
            }
            kTest.info("queued behind the stall");
            fv::metrics::record("shutdown_s", 0.5);
            const auto started = std::chrono::steady_clock::now();
            logging = fv::log::LoggingHandle();
            took = std::chrono::steady_clock::now() - started;
        }
        {
            std::lock_guard lock(stall->mutex);
            stall->released = true;
            stall->changed.notify_all();
        }
        return took <= std::chrono::milliseconds(1500) ? 0 : 4;
    }

    fv::log::CrashHandler::install(paths, fv::identity());
    if (mode == "access-violation") {
        *g_target = 1;
    } else if (mode == "terminate") {
        std::terminate();
    }
#if defined(_WIN32)
    else if (mode == "watcher-gone") {
        if (!end_crash_watcher()) {
            return 6;
        }
        *g_target = 1;
    }
#endif
    return 3;
}
