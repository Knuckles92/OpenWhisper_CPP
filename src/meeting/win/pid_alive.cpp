#include <windows.h>

#include <cstdint>
#include <limits>

#include "meeting/recovery.h"

namespace fv::meeting {

bool pid_alive(std::int64_t pid) {
    if (pid <= 0 || pid > std::numeric_limits<DWORD>::max()) {
        return false;
    }
    // A read-only probe: os.kill(pid, 0) would terminate the target on Windows.
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        // Access denied: the process exists but is protected.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD exit_code = 0;
    const BOOL read = GetExitCodeProcess(handle, &exit_code);
    CloseHandle(handle);
    return read != FALSE && exit_code == STILL_ACTIVE;
}

} // namespace fv::meeting
