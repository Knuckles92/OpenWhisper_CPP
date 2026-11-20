#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "base/env.h"

// Python's os.path.expanduser and os.path.abspath on UTF-8 paths, with the environment injected.
namespace fv::detail {

// ntpath.expanduser (USERPROFILE, else HOMEDRIVE + HOMEPATH; "~user" guessed beside the current
// user's home) on Windows, posixpath.expanduser (HOME, else the password database) elsewhere.
// Python 3.12's rules; the path comes back unchanged when no home can be found. The password
// database is consulted only for the process's own environment (EnvReader::is_process).
std::string expanduser(std::string_view path, const EnvReader& env);
std::string expanduser_nt(std::string_view path, const EnvReader& env);
std::string expanduser_posix(std::string_view path, const EnvReader& env);

// The home directory in the password database: of the current user when `user` is nullopt.
// Always nullopt on Windows.
std::optional<std::string> passwd_home(std::optional<std::string_view> user);

// os.path.abspath: absolute against the working directory, "." and ".." folded, separators
// made native, no trailing separator except on a root.
std::string abspath(std::string_view path);

} // namespace fv::detail
