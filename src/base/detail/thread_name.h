#pragma once

#include <string>

namespace fv::detail {

// Names the calling thread for debuggers and crash dumps. Linux keeps the first 15 bytes.
void set_current_thread_name(const std::string& name);

} // namespace fv::detail
