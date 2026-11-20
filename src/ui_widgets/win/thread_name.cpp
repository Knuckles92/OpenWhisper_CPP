#include "ui_widgets/detail/thread_name.h"

#include <windows.h>

#include <string>

namespace fv::ui_widgets_detail {

void setCurrentThreadName(const std::string& name) {
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(), size);
    // A name is a debugging aid: a failure changes nothing else.
    (void)SetThreadDescription(GetCurrentThread(), wide.c_str());
}

} // namespace fv::ui_widgets_detail
