#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "base/env.h"

// The os.path and pathlib calls the knowledge folder makes, with this platform's semantics
// (ntpath on Windows, posixpath elsewhere), on UTF-8 text.
namespace fv::meeting_intel::detail {

// os.path.expanduser over `env`: on Windows USERPROFILE, else HOMEDRIVE + HOMEPATH, with "~user"
// guessed beside the current user's home (USERNAME); elsewhere HOME. A path without a home to
// expand to comes back unchanged.
std::string expanduser(std::string_view path, const EnvReader& env);
// os.path.isabs.
bool isabs(std::string_view path);
// os.path.normpath: separators made native, "." and empty parts dropped, ".." folded where a
// parent exists.
std::string normpath(std::string_view path);
// os.path.abspath: absolute against the working directory, then normalized.
std::string abspath(std::string_view path);

// os.path.islink: a symbolic link (a Windows junction is not one), without following it.
bool islink(const std::filesystem::path& path);
// Path.resolve(strict=True): links followed, every part must exist; nullopt on any failure.
std::optional<std::filesystem::path> resolve_strict(const std::filesystem::path& path);
// Path.resolve(strict=False): links followed as far as the path exists.
std::optional<std::filesystem::path> resolve_lax(const std::filesystem::path& path);
// The parts of `path` below `root`, compared as Python's PurePath does (case-insensitively on
// Windows); nullopt when `path` is not `root` or below it.
std::optional<std::filesystem::path> relative_to(const std::filesystem::path& path,
                                                 const std::filesystem::path& root);

} // namespace fv::meeting_intel::detail
