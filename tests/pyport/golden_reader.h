// A small reader of tests/golden/builtins.json (the parity harness's vector format), so the
// pyport vectors replay without waiting for fv_golden.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "pyport/json.h"

namespace golden {

using fv::py::Json;

// The cases of one function of builtins.json; fails the test when there are none.
const Json& cases(std::string_view function);
// Every function name in the file.
std::vector<std::string> function_names();

// A vector value with its tags resolved: {"$float": "nan"} to a double, {"$int": digits} to an
// unsigned integer when it fits (else left tagged), {"$code_points": [...]} to its UTF-8 (a
// lone surrogate in three bytes).
Json decode(const Json& value);

// Equal in value and kind: int and float differ, floats compare bit for bit (nan equals nan,
// -0.0 differs from 0.0), signed and unsigned integers compare by value.
bool same(const Json& expected, const Json& actual);

// Runs check(args) for every case of function, with the case shown on failure. Where the case
// raises, check must throw fv::py::PyError of that Python type (and message, when recorded);
// otherwise check returns the result, which must be same() as the recorded one.
void replay(std::string_view function, const std::function<Json(const Json& args)>& check);

} // namespace golden
