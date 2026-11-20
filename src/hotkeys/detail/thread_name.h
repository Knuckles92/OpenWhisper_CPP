#pragma once

#include <string>

namespace fv::hotkeys::detail {

// Names the calling thread for debuggers and crash dumps; does nothing where unsupported.
void name_current_thread(const std::string& name);

} // namespace fv::hotkeys::detail
