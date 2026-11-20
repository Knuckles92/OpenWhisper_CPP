#pragma once

#include <string_view>

namespace fv::automation::detail {

// Names the calling thread for debuggers and crash dumps, as Python names the bus's dispatch
// thread ("automation-events"). Linux keeps the first 15 bytes.
void set_current_thread_name(std::string_view name);

} // namespace fv::automation::detail
