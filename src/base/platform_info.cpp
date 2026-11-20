#include "base/platform_info.h"

#include "base/detail/platform_probe.h"

namespace fv {

std::string_view python_platform() {
#if defined(_WIN32)
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "linux";
#endif
}

std::optional<std::string> windows_machine_name(std::uint16_t image_file_machine) {
    switch (image_file_machine) {
    case 0x014C:
        return "x86";
    case 0x01C4:
        return "ARM";
    case 0x0200:
        return "ia64";
    case 0x8664:
        return "AMD64";
    case 0xAA64:
        return "ARM64";
    default:
        return std::nullopt;
    }
}

std::optional<std::string> windows_native_machine() {
    const auto machine = detail::kernel_native_machine();
    if (!machine) {
        return std::nullopt;
    }
    return windows_machine_name(*machine);
}

// config.py:56-71: `_windows_native_machine() or PROCESSOR_ARCHITEW6432 or
// PROCESSOR_ARCHITECTURE`, where `or` also skips an empty answer.
std::string host_machine(const HostMachineProbe& probe) {
    if (!probe.platform.starts_with("win")) {
        return probe.uname_machine ? probe.uname_machine() : std::string();
    }
    if (probe.windows_native_machine) {
        if (auto native = probe.windows_native_machine(); native && !native->empty()) {
            return *native;
        }
    }
    if (probe.env) {
        if (auto wow64 = probe.env->get("PROCESSOR_ARCHITEW6432"); wow64 && !wow64->empty()) {
            return *wow64;
        }
        return probe.env->get("PROCESSOR_ARCHITECTURE").value_or("");
    }
    return {};
}

std::string host_machine() {
    return host_machine(HostMachineProbe{
        .platform = python_platform(),
        .windows_native_machine = [] { return windows_native_machine(); },
        .uname_machine = [] { return detail::uname_machine(); },
        .env = &system_env(),
    });
}

} // namespace fv
