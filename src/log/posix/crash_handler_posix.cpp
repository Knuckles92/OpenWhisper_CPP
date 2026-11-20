#include "log/crash_handler.h"

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

#include "log/detail/crash_line.h"

namespace fv::log {

namespace {

// Allocated at install: a signal handler may not allocate.
std::string* g_log_path = nullptr;
char g_line[512];
std::once_flag g_install;

void on_signal(int number) {
    if (g_log_path) {
        const auto fields = detail::utc_fields(static_cast<std::int64_t>(::time(nullptr)));
        detail::LineBuffer line(g_line, sizeof(g_line));
        detail::put_crash_line(line, fields, static_cast<std::uint32_t>(number), {}, {});
        const int fd = ::open(g_log_path->c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0) {
            [[maybe_unused]] const auto written = ::write(fd, g_line, line.size());
            ::close(fd);
        }
    }
    // SA_RESETHAND restored the default action: re-raise to end the process as it would have.
    ::raise(number);
}

} // namespace

void CrashHandler::install(const Paths& paths, const AppIdentity& identity) {
    const auto log_path = crash_log_file(paths, identity);
    std::filesystem::create_directories(log_path.parent_path());
    if (!std::ofstream(log_path, std::ios::binary | std::ios::app)) {
        throw std::filesystem::filesystem_error("Cannot open the crash log", log_path,
                                                std::make_error_code(std::errc::io_error));
    }
    std::call_once(g_install, [&] {
        g_log_path = new std::string(log_path.native());
        struct sigaction action {};
        action.sa_handler = on_signal;
        action.sa_flags = SA_RESETHAND | SA_NODEFER;
        sigemptyset(&action.sa_mask);
        for (const int number : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) {
            ::sigaction(number, &action, nullptr);
        }
    });
}

} // namespace fv::log
