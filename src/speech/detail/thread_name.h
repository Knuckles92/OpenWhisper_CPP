#pragma once

#include <string_view>

namespace fv::speech::detail {

// Names the calling thread for debuggers and crash dumps ("fv-speech").
void set_current_thread_name(std::string_view name);

} // namespace fv::speech::detail
