#pragma once

#include <string_view>

#include "base/json.h"
#include "models/catalog.h"

namespace fv::remote_host {

// What a paired client can see of the host's bundled Parakeet runtimes, including sizes and
// availability. The host catalog alone determines which components can be offered.
Json runtime_dependencies(const models::Host& host);
Json runtime_state(const Json& engine, const Json& settings, const models::Host& host);

// Validate the requested changes against the current engine and the actual host choices. The
// returned patch contains settings keys and is safe to merge only while that engine is current.
Json validate_runtime(const Json& engine, const Json& settings, const models::Host& host,
                      std::string_view family, std::string_view model, const Json& changes);

} // namespace fv::remote_host
