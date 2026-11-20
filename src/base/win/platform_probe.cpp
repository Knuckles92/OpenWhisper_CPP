#include "base/detail/platform_probe.h"

#include <windows.h>

namespace fv::detail {

std::optional<std::uint16_t> kernel_native_machine() {
    using Query = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == nullptr) {
        return std::nullopt;
    }
    // Looked up rather than linked: Python's ctypes call tolerates a kernel32 without it.
    const auto query = reinterpret_cast<Query>(
        reinterpret_cast<void*>(GetProcAddress(kernel32, "IsWow64Process2")));
    if (query == nullptr) {
        return std::nullopt;
    }
    USHORT process_machine = 0;
    USHORT native_machine = 0;
    if (!query(GetCurrentProcess(), &process_machine, &native_machine)) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(native_machine);
}

std::string uname_machine() {
    return {};
}

} // namespace fv::detail
