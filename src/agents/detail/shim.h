#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// npm's cmd-shim on Windows, read without running it (agents.py npm_shim_command, _package_exe).
namespace fv::agents::detail {

// How much of a shim is read, in bytes.
inline constexpr std::size_t SHIM_READ_MAX = 16'384;

// npm_shim_command with node.exe looked up on `path_env` (the user's PATH, as shutil.which reads
// it; nullopt: no PATH).
std::optional<std::vector<std::string>>
npm_shim_command(const std::filesystem::path& shim, const std::optional<std::string>& path_env);

// The program and script a shim's text names: (every _prog value, the script relative to the
// shim's folder with backslashes), or nullopt when it is not npm's shim of a Node script.
struct ShimTarget {
    std::vector<std::string> programs;
    std::string script;
};
std::optional<ShimTarget> parse_npm_shim(std::string_view source);

// `command`.exe that the npm package holding `script` ships, or "" (_package_exe).
std::filesystem::path package_exe(const std::filesystem::path& script, std::string_view command,
                                  const std::filesystem::path& npm_folder);

} // namespace fv::agents::detail
