#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>

#include "meeting/detail/engine_internal.h"
#include "meeting/engine.h"

namespace fv::meeting {

std::int64_t current_process_id() {
    return static_cast<std::int64_t>(GetCurrentProcessId());
}

namespace detail {

void name_engine_thread(const std::string& name) {
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(),
                            length);
    }
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

} // namespace detail

} // namespace fv::meeting
