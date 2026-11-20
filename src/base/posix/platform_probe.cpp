#include "base/detail/platform_probe.h"

#include <sys/utsname.h>

namespace fv::detail {

std::optional<std::uint16_t> kernel_native_machine() {
    return std::nullopt;
}

std::string uname_machine() {
    utsname info{};
    if (uname(&info) != 0) {
        return {};
    }
    return info.machine;
}

} // namespace fv::detail
