#include "focus/snapshot.h"

namespace fv::focus {

bool same_target(const std::optional<AppIdentity>& current,
                 const std::optional<AppIdentity>& expected) {
    if (!current || !expected) {
        return false;
    }
    return current->app_id == expected->app_id && current->pid == expected->pid &&
           current->window == expected->window && current->title_hint == expected->title_hint;
}

} // namespace fv::focus
