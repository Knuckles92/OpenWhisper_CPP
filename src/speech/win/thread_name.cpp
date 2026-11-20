#include "speech/detail/thread_name.h"

#include <string>

#include <windows.h>

namespace fv::speech::detail {

void set_current_thread_name(std::string_view name) {
    // The names are ASCII.
    const std::wstring wide(name.begin(), name.end());
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

} // namespace fv::speech::detail
