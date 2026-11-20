#include "meeting_intel/detail/report.h"

#include <string>

#include <windows.h>

namespace fv::meeting_intel::detail {

void set_current_thread_name(std::string_view name) {
    // The names are ASCII literals.
    const std::wstring wide(name.begin(), name.end());
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

} // namespace fv::meeting_intel::detail
