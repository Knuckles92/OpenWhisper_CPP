#pragma once

#include <string>

namespace fv::ui_widgets_detail {

// Names the calling thread for debuggers and crash dumps, as Python names its worker threads.
// Linux keeps the first 15 bytes.
void setCurrentThreadName(const std::string& name);

} // namespace fv::ui_widgets_detail
