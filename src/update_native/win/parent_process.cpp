#include "update_native/parent_process.h"
#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "update_native/manifest.h"

namespace fv::update_native {
namespace {
struct Handle {
    HANDLE value;
    ~Handle() {
        if (value)
            CloseHandle(value);
    }
};
} // namespace
bool wait_for_parent(std::uint32_t pid, const std::filesystem::path& expected_exe,
                     std::uint64_t creation_ticks, std::chrono::milliseconds timeout,
                     const CancelToken& cancel) {
    if (!pid || !expected_exe.is_absolute() || !creation_ticks || timeout.count() < 0)
        throw std::invalid_argument("Invalid update parent identity.");
    Handle process{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process.value) {
        if (GetLastError() == ERROR_INVALID_PARAMETER)
            return true;
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Opening update parent process");
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process.value, &created, &exited, &kernel, &user))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Reading update parent creation time");
    const auto ticks =
        (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    std::vector<wchar_t> image(32768);
    DWORD size = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process.value, 0, image.data(), &size))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Reading update parent image");
    const auto actual = std::filesystem::path(std::wstring(image.data(), size)).lexically_normal();
    const auto expected = expected_exe.lexically_normal();
    if (ticks != creation_ticks ||
        CompareStringOrdinal(actual.c_str(), -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL)
        throw std::runtime_error("The update parent process identity changed.");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (cancel.cancelled())
            return false;
        const auto now = std::chrono::steady_clock::now();
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto wait = now >= deadline ? 0 : std::min<std::int64_t>(remaining.count() + 1, 100);
        const DWORD result = WaitForSingleObject(process.value, static_cast<DWORD>(wait));
        if (result == WAIT_OBJECT_0)
            return true;
        if (result != WAIT_TIMEOUT)
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Waiting for update parent process");
        if (now >= deadline)
            return false;
    }
}
std::int64_t current_process_creation_time() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        throw UpdateError("Could not identify the running FioriVoce process.");
    ULARGE_INTEGER ticks;
    ticks.LowPart = created.dwLowDateTime;
    ticks.HighPart = created.dwHighDateTime;
    return static_cast<std::int64_t>(ticks.QuadPart);
}
} // namespace fv::update_native
#endif
