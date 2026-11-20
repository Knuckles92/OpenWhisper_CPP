#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "update/install_probe.h"

// The operating-system reads of the update module: win/ implements them, posix/ finds nothing.
namespace fv::update::detail {

// The values of one uninstall key, each as str() of what the registry holds ("" when absent).
struct UninstallValues {
    std::string install_location, uninstall_string, display_version, estimated_size, install_date;
};

// nullopt when the key cannot be opened under that hive and view.
std::optional<UninstallValues> read_uninstall_key(RegistryHive, std::string_view key_path,
                                                  int view);

// The views to try, in order: 0 (the default), then KEY_WOW64_64KEY and KEY_WOW64_32KEY.
std::vector<int> registry_views();

// The running executable's absolute path.
std::filesystem::path executable_path();

// "<file index>:<size>:<modification time in ns>" of a file, as os.stat's st_ino, st_size and
// st_mtime_ns read on Windows; nullopt when it cannot be read. Opens the file for its attributes
// only, sharing everything, and never starts a process.
std::optional<std::string> file_identity(const std::filesystem::path&);

} // namespace fv::update::detail
