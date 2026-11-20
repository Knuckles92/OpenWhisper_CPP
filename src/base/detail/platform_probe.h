#pragma once

#include <cstdint>
#include <optional>
#include <string>

// What the operating system reports about the CPU, one implementation per platform.
namespace fv::detail {

// IsWow64Process2's native IMAGE_FILE_MACHINE_* value; nullopt off Windows, or when kernel32
// lacks the call (before Windows 10 1709) or it fails.
std::optional<std::uint16_t> kernel_native_machine();

// uname()'s machine field; "" on Windows and when uname fails.
std::string uname_machine();

} // namespace fv::detail
