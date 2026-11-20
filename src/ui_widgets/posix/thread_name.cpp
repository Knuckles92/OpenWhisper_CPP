#include "ui_widgets/detail/thread_name.h"

#if defined(__linux__)
#include <pthread.h>
#endif

#include <string>

namespace fv::ui_widgets_detail {

void setCurrentThreadName(const std::string& name) {
#if defined(__linux__)
    // The kernel keeps 15 bytes and a terminator; a longer name is refused, not cut.
    (void)pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#else
    (void)name;
#endif
}

} // namespace fv::ui_widgets_detail
