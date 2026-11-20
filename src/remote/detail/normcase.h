#pragma once

#include <string>
#include <string_view>

namespace fv::remote::detail {

// os.path.normcase of a UTF-8 path: on Windows "/" as "\" and the text lowercased as
// LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE) does; elsewhere unchanged.
std::string normcase(std::string_view path);

} // namespace fv::remote::detail
