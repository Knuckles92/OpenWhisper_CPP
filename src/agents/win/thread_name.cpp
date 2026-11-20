#include "agents/detail/thread_name.h"

#include <string>

#include <windows.h>

namespace fv::agents::detail {

void set_current_thread_name(std::string_view name) {
    // The names are ASCII (agent ids and a counter).
    const std::wstring wide(name.begin(), name.end());
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

} // namespace fv::agents::detail
