#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// `openwhisper-cpp-cli --self-test --installed`: the install folder holds every file the programs
// need (a staged or installed tree; windeployqt decides the Qt plugin set, so the ones the app
// cannot start or look right without are checked by name).
namespace fv::cli {

// What an install folder must hold. Paths are relative, with "/" separators.
struct InstalledLayout {
    // Files that must be there.
    std::vector<std::string> required;
    // Programs and libraries whose imports are followed: every DLL they import, and the DLLs
    // those import in turn, must be in the folder unless it is a system library.
    std::vector<std::string> import_roots;
    // Lower-case name prefixes of libraries that must be in the folder even when the system
    // has them (the app-local C++ runtime).
    std::vector<std::string> local_only_prefixes;
};

// The Windows install layout: openwhisper-cpp.exe and openwhisper-cpp-cli.exe, the Qt platform,
// style, image format, icon engine and Schannel TLS plugins, the Qt6 Core, Gui, Widgets, Svg and
// Network DLLs, the app-local C++ runtime and THIRD_PARTY_NOTICES.md. Elsewhere throws
// std::runtime_error (not supported on this platform).
InstalledLayout installed_layout();

// Whether the loader finds the DLL `name` in the system: an API set, or a library Windows itself
// ships (a fixed list) that is in `system_folder`. Any other library there, such as a zlib DLL a
// third-party installer put in System32, does not count, so the install must hold its own. False
// outside Windows.
bool is_system_library_in(std::string_view name, const std::filesystem::path& system_folder);
// is_system_library_in the system folder (GetSystemDirectoryW).
bool is_system_library(std::string_view name);

// The folder of the running executable. Throws std::runtime_error when the system does not say.
std::filesystem::path executable_folder();

// One file the folder lacks: its relative path (with the platform's separators), and the
// program or library that imports it, empty for a file the layout names.
struct MissingFile {
    std::string path;
    std::string imported_by;

    bool operator==(const MissingFile&) const = default;
};

// Every file of `layout` missing from `folder`, the layout's own files first, then the imported
// libraries in the order the walk meets them; each name once. A program or library that is
// there but cannot be read as one is reported with imported_by "(not a Windows executable)".
std::vector<MissingFile>
missing_installed_files(const std::filesystem::path& folder, const InstalledLayout& layout,
                        const std::function<bool(std::string_view)>& is_system);

// The command: checks the executable's folder, prints "<product> installed self-test passed" and
// returns 0, or lists every missing file on stderr and returns 1.
int run_installed_self_test();

} // namespace fv::cli
