#include "update_native/parent_process.h"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include "update_native/manifest.h"

// The updater ships for Windows only. Linux identifies a process by /proc so the shared recovery
// code can be exercised; any other system cannot tell and fails closed.
namespace fv::update_native {
namespace {
namespace fs = std::filesystem;

// Field 22 of /proc/<pid>/stat: the start time in clock ticks since boot. The command (field 2)
// may contain spaces and parentheses, so counting starts after its closing parenthesis.
std::int64_t start_ticks(const std::string& pid) {
    std::ifstream in("/proc/" + pid + "/stat");
    std::string line;
    if (!std::getline(in, line))
        return 0;
    auto at = line.rfind(')');
    if (at == std::string::npos)
        return 0;
    for (int field = 2; field < 22; ++field) {
        at = line.find(' ', at + 1);
        if (at == std::string::npos)
            return 0;
    }
    try {
        return std::stoll(line.substr(at + 1));
    } catch (const std::exception&) {
        return 0;
    }
}

bool alive(std::uint32_t pid) {
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
}
} // namespace

bool wait_for_parent(std::uint32_t pid, const fs::path& expected_exe, std::uint64_t creation_ticks,
                     std::chrono::milliseconds timeout, const CancelToken& cancel) {
    if (!pid || !expected_exe.is_absolute() || !creation_ticks || timeout.count() < 0)
        throw std::invalid_argument("Invalid update parent identity.");
    if (!alive(pid))
        return true;
    std::error_code error;
    const auto actual = fs::read_symlink("/proc/" + std::to_string(pid) + "/exe", error);
    const auto ticks = start_ticks(std::to_string(pid));
    if (error || ticks <= 0) {
        if (!alive(pid))
            return true;
        throw std::system_error(error ? error : std::make_error_code(std::errc::not_supported),
                                "Reading update parent identity");
    }
    if (static_cast<std::uint64_t>(ticks) != creation_ticks ||
        actual.lexically_normal() != expected_exe.lexically_normal())
        throw std::runtime_error("The update parent process identity changed.");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (!alive(pid))
            return true;
        if (cancel.cancelled())
            return false;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
            deadline - now, std::chrono::milliseconds(100)));
    }
}

std::int64_t current_process_creation_time() {
    const auto ticks = start_ticks("self");
    if (ticks <= 0)
        throw UpdateError("Could not identify the running FioriVoce process.");
    return ticks;
}
} // namespace fv::update_native
