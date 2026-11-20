#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

namespace fv::net {

// discovery.sweep's knock: socket.create_connection((address, port), timeout), closed at once.
// True when something accepted the connection. Each address the name resolves to gets `timeout`
// (the resolving itself is not bounded, as in Python). Blocking: a worker thread's.
bool tcp_knock(std::string_view address, std::uint16_t port, std::chrono::milliseconds timeout);

} // namespace fv::net
