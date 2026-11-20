#pragma once

#include <cstdint>
#include <string>

// The UDP socket calls that differ between Windows (win/) and POSIX (posix/). No Boost here.
namespace fv::net::detail {

// Windows: WSAIoctl(SIO_UDP_CONNRESET, FALSE), so an ICMP "port unreachable" after a send no
// longer fails the next receive. Elsewhere nothing (POSIX reports it only on connected sockets).
void disable_udp_connreset(std::uintptr_t native_socket);

// OSError.strerror for an OS error code as Python builds it: FormatMessageW in the system's
// language on Windows, strerror elsewhere, without a trailing period or line end.
std::string os_error_text(int code);

} // namespace fv::net::detail
