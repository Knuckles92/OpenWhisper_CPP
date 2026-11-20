#pragma once

#include <string>

namespace fv::agentapi::detail {

// socket.gethostname(): this computer's host name (on Windows its physical DNS host name, as
// CPython asks GetComputerNameExW), UTF-8; "" when it cannot be read.
std::string this_host_name();

} // namespace fv::agentapi::detail
