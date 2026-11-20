#pragma once

#include <string_view>

#include "base/json.h"

namespace fv::backup::detail {

// json.loads of bytes: the encoding detected as json.detect_encoding detects it (a UTF-32,
// UTF-16 or UTF-8 byte-order mark, else the pattern of NUL bytes, else UTF-8) and decoded with
// "surrogatepass". Throws py::ValueError for bytes that do not decode (UnicodeDecodeError is a
// ValueError) and what py::loads throws for the text.
Json loads_bytes(std::string_view bytes);

} // namespace fv::backup::detail
