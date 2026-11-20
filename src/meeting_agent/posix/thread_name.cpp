#include "meeting_agent/detail/scheduler_support.h"

#include <string>

#include <pthread.h>

namespace fv::meeting_agent::detail {

void set_thread_name(std::string_view name) {
    // Linux limits a thread name to 15 bytes.
    const std::string shortened(name.substr(0, 15));
    pthread_setname_np(pthread_self(), shortened.c_str());
}

} // namespace fv::meeting_agent::detail
