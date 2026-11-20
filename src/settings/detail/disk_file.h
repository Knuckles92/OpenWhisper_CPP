#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "settings/settings_file.h"

// The platform half of disk_settings_file (win/ and posix/).
namespace fv::detail {

// os.stat of `absolute`: exists == false when the file (or a folder on its path) is missing,
// nullopt for any other failure.
std::optional<FileSignature> stat_signature(const std::filesystem::path& absolute);

// open(path, "rb").read(), with Python's OSError text on failure: "[Errno 13] Permission
// denied: '<path>'" and its PermissionError, FileNotFoundError, IsADirectoryError or OSError type.
std::string read_whole_file(const std::filesystem::path& path);

} // namespace fv::detail
