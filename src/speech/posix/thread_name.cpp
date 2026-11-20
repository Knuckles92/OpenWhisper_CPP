#include "speech/detail/thread_name.h"

#include <string>

#include <pthread.h>

namespace fv::speech::detail {

void set_current_thread_name(std::string_view name) {
#if defined(__APPLE__)
    pthread_setname_np(std::string(name).c_str());
#else
    // Linux keeps at most 15 characters.
    pthread_setname_np(pthread_self(), std::string(name.substr(0, 15)).c_str());
#endif
}

} // namespace fv::speech::detail
