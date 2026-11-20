#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Text crosses the engine as UTF-8 std::string. These helpers are the only
// places that meet the platform's native encoding.
namespace fv {

std::filesystem::path path_from_utf8(std::string_view text);
std::string path_to_utf8(const std::filesystem::path& path);

// The environment variable's value, or nullopt when it is unset or empty.
std::optional<std::string> env_utf8(const char* name);

// The process arguments as UTF-8. On Windows argv is in the ANSI code page and
// loses characters outside it, so this reads the wide command line instead.
std::vector<std::string> utf8_args(int argc, char** argv);

// Make the console print UTF-8 (a no-op outside Windows).
void use_utf8_console();

// Windows: adds `folder` to the process's DLL search path (AddDllDirectory, which Python's
// os.add_dll_directory calls), so libraries a runtime loads by name after it starts find their
// siblings there. It stays for the life of the process, as the runtime does. Elsewhere a no-op.
void add_dll_directory(const std::filesystem::path& folder);

} // namespace fv
