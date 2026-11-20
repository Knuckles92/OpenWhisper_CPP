#include "agentapi/detail/host_name.h"

#include <climits>
#include <string>

#include <unistd.h>

namespace fv::agentapi::detail {

std::string this_host_name() {
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) != 0) {
        return {};
    }
    return name;
}

} // namespace fv::agentapi::detail
