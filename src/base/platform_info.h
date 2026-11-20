#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "base/env.h"

namespace fv {

// This build's platform as Python's sys.platform names it: "win32", "linux" or "darwin".
std::string_view python_platform();

// This host's CPU architecture as Python's platform.machine() names it ("AMD64", "ARM64", "x86"
// on Windows; uname's machine elsewhere). Windows never asks WMI: Python 3.12's
// platform.machine() does, and an unhealthy WMI service killed the process (0xC000070A). It asks
// the kernel, then falls back to PROCESSOR_ARCHITEW6432 and PROCESSOR_ARCHITECTURE, as CPython
// itself does; "" when none of them answers.
std::string host_machine();

// The kernel's answer: IsWow64Process2's native machine, which names the host even for an x64
// build emulated on ARM64. nullopt off Windows, when the call is unavailable or fails, or for a
// machine without a name below.
std::optional<std::string> windows_native_machine();

// IMAGE_FILE_MACHINE_* named the way platform.machine() names it: 0x014C "x86", 0x01C4 "ARM",
// 0x0200 "ia64", 0x8664 "AMD64", 0xAA64 "ARM64".
std::optional<std::string> windows_machine_name(std::uint16_t image_file_machine);

// host_machine with every input injected (tests fake the kernel).
struct HostMachineProbe {
    std::string_view platform; // sys.platform
    std::function<std::optional<std::string>()> windows_native_machine;
    std::function<std::string()> uname_machine; // platform.machine() off Windows
    const EnvReader* env = nullptr;
};
std::string host_machine(const HostMachineProbe&);

} // namespace fv
