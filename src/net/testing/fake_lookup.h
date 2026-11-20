#pragma once

#include <expected>
#include <functional>
#include <string>
#include <vector>

#include "net/address.h"

namespace fv::net::testing {

using Lookup =
    std::function<std::expected<std::vector<IpAddress>, std::string>(const std::string&)>;

// Replaces the getaddrinfo that resolve_host makes, for this object's life: a test plays a name
// server that never answers. The lookup thread holds its own copy of `lookup`, so whatever it
// captures must stay valid until it returns (share it). One at a time.
class ScopedLookup {
public:
    explicit ScopedLookup(Lookup lookup);
    ~ScopedLookup();
    ScopedLookup(const ScopedLookup&) = delete;
    ScopedLookup& operator=(const ScopedLookup&) = delete;
};

} // namespace fv::net::testing
