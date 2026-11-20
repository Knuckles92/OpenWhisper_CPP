#include "hotkeys/detail/thread_name.h"

#include <windows.h>

#include "base/utf8.h"

namespace fv::hotkeys::detail {

void name_current_thread(const std::string& name) {
    const std::wstring wide = path_from_utf8(name).wstring();
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

} // namespace fv::hotkeys::detail
