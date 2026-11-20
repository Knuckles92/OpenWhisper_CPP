#pragma once

#include <string>
#include <string_view>

#include "base/json.h"

namespace fv::llm::detail {

// repr() of the Python value json.loads makes of `value`: {'a': [1, True, None]}.
std::string py_repr(const Json& value);
// str() of it: a string as it is, anything else as py_repr.
std::string py_str(const Json& value);
// type(value).__name__: "dict", "list", "str", "int", "float", "bool", "NoneType".
std::string_view py_type_name(const Json& value);

} // namespace fv::llm::detail
