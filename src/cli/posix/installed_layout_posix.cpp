#include "cli/installed_check.h"

#include <stdexcept>
#include <system_error>

namespace fv::cli {

// The install layout is the Windows setup's; no other platform has an installer yet.
InstalledLayout installed_layout() {
    throw std::runtime_error("The installed self-test is not supported on this platform.");
}

bool is_system_library_in(std::string_view, const std::filesystem::path&) {
    return false;
}

bool is_system_library(std::string_view) {
    return false;
}

std::filesystem::path executable_folder() {
    std::error_code error;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        throw std::runtime_error("The executable's folder could not be found.");
    }
    return self.parent_path();
}

} // namespace fv::cli
