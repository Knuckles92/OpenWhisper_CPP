#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "net/address.h"

// The few socket calls that differ between Windows (win/) and POSIX (posix/). No Boost here.
namespace fv::net::detail {

void set_thread_name(const std::string& name);

// Before bind: SO_EXCLUSIVEADDRUSE on Windows when `exclusive` (never SO_REUSEADDR there, or a
// second process could share the port); SO_REUSEADDR elsewhere, as uvicorn binds.
std::error_code set_address_reuse(std::uintptr_t native_socket, bool exclusive);

// getaddrinfo(name, nullptr, {AF_UNSPEC, SOCK_STREAM}); blocking. Names beyond ASCII go
// through the system's IDN conversion.
std::expected<std::vector<IpAddress>, std::string> getaddrinfo_stream(const std::string& name);

// What resolve_host calls instead of getaddrinfo_stream while set (net/testing/fake_lookup.h);
// empty: getaddrinfo_stream. Defined in address.cpp.
void set_lookup_override(
    std::function<std::expected<std::vector<IpAddress>, std::string>(const std::string&)> lookup);

// socket(family, SOCK_STREAM) and bind((address, 0)).
bool probe_bind(const IpAddress& address);

} // namespace fv::net::detail
