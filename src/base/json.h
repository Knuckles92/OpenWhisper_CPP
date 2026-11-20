#pragma once

#include <nlohmann/json.hpp>

namespace fv {

// Insertion order and the int/float/bool distinction are kept, as Python's json does.
using Json = nlohmann::ordered_json;

} // namespace fv
