#include "base/detail/thread_name.h"

#include <windows.h>

#include "base/detail/win_text.h"

namespace fv::detail {

void set_current_thread_name(const std::string& name) {
    const std::wstring wide = widen(name);
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

} // namespace fv::detail
