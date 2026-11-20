#include <cstdint>
#include <string>

#include <pthread.h>
#include <unistd.h>

#include "meeting/detail/engine_internal.h"
#include "meeting/engine.h"

namespace fv::meeting {

std::int64_t current_process_id() {
    return static_cast<std::int64_t>(::getpid());
}

namespace detail {

void name_engine_thread(const std::string& name) {
    // The kernel's limit is 16 bytes including the terminator; longer names are refused.
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
}

} // namespace detail

} // namespace fv::meeting
