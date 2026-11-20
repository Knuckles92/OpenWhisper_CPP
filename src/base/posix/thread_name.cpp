#include "base/detail/thread_name.h"

#include <pthread.h>

namespace fv::detail {

void set_current_thread_name(const std::string& name) {
#if defined(__APPLE__)
    pthread_setname_np(name.substr(0, 63).c_str());
#else
    // The kernel's limit is 16 bytes including the terminator; longer names are refused.
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#endif
}

} // namespace fv::detail
