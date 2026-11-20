#pragma once

#include <string>
#include <string_view>

namespace fv::bench::detail {

// unicodedata.normalize("NFKC", text) of Python 3.12 (Unicode 15.0.0): compatibility
// decomposition, canonical ordering, then canonical composition, Hangul included. Invalid UTF-8
// decodes as py::to_code_points does.
std::string nfkc(std::string_view text);

} // namespace fv::bench::detail
