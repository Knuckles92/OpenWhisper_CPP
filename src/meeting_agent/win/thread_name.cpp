#include "meeting_agent/detail/scheduler_support.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace fv::meeting_agent::detail {

void set_thread_name(std::string_view name) {
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(),
                            length);
    }
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

} // namespace fv::meeting_agent::detail
