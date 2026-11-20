#include "net/testing/fake_lookup.h"

#include <utility>

#include "net/detail/platform.h"

namespace fv::net::testing {

ScopedLookup::ScopedLookup(Lookup lookup) {
    detail::set_lookup_override(std::move(lookup));
}

ScopedLookup::~ScopedLookup() {
    detail::set_lookup_override({});
}

} // namespace fv::net::testing
