#pragma once

#include <string>

namespace fv::http::detail {

// str() of the OSError Python's socket module raises for a system error code: on Windows
// "[WinError 10061] No connection could be made because the target machine actively refused
// it", elsewhere "[Errno 111] Connection refused".
std::string socket_error_text(long code);

// str() of the socket.gaierror a failed name lookup raises.
std::string dns_failure_text();

} // namespace fv::http::detail
