#pragma once

#include <filesystem>
#include <string>

namespace fv::download::detail {

#ifdef _WIN32
inline constexpr bool kWindows = true;
#else
inline constexpr bool kWindows = false;
#endif

// The system's text for a system_category code, as Python puts it after "[WinError N] " (trailing
// dots and white space trimmed) or "[Errno N] ".
std::string system_message(int code);

// Persists a rename in `directory` (POSIX); Windows cannot open a directory to flush it.
void sync_directory(const std::filesystem::path& directory);

} // namespace fv::download::detail
