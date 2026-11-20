#include "meeting_web/dashboard_root.h"

namespace fv::meeting_web {

std::filesystem::path default_dist_root() {
    const std::filesystem::path dir = executable_dir();
    if (dir.empty()) {
        return {};
    }
    return dir / "webui" / "dist";
}

} // namespace fv::meeting_web
