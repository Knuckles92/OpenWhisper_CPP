#include <cerrno>
#include <cstdint>
#include <limits>

#include <signal.h>
#include <sys/types.h>

#include "meeting/recovery.h"

namespace fv::meeting {

bool pid_alive(std::int64_t pid) {
    if (pid <= 0 || pid > std::numeric_limits<pid_t>::max()) {
        return false;
    }
    // Signal 0 checks for existence without delivering anything.
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    // EPERM: it exists, owned by someone else.
    return errno == EPERM;
}

} // namespace fv::meeting
