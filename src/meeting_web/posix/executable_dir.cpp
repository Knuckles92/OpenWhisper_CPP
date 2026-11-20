#include <system_error>

#include "meeting_web/dashboard_root.h"

namespace fv::meeting_web {

std::filesystem::path executable_dir() {
    std::error_code error;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        return {};
    }
    return self.parent_path();
}

} // namespace fv::meeting_web
