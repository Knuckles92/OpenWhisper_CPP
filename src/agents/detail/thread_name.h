#pragma once

#include <string_view>

namespace fv::agents::detail {

// Names the calling thread for debuggers and crash dumps, as Python names the scan and lookup
// threads ("agent-scan_<n>", "agent-lookup-<id>"). Linux keeps the first 15 bytes.
void set_current_thread_name(std::string_view name);

} // namespace fv::agents::detail
