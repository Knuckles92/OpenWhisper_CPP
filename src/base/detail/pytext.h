#pragma once

#include <string>
#include <string_view>
#include <vector>

// The few Python str methods base needs on UTF-8 text. fv_pyport has the full set; base cannot
// depend on it.
namespace fv::detail {

// str.lower() and str.upper() as far as comparing the result with an ASCII word goes: A-Z and
// a-z change, and upper() also maps U+0131 (dotless i) to "I" and U+017F (long s) to "S", the two
// non-ASCII letters Python uppercases into ASCII. Other text passes through unchanged.
std::string ascii_lower(std::string_view text);
std::string ascii_upper(std::string_view text);

// str.strip() with no argument: removes the characters str.isspace() accepts from both ends.
std::string_view py_strip(std::string_view text);

// str.splitlines(): splits on \n, \r, \r\n, \v, \f, \x1c-\x1e, U+0085, U+2028 and U+2029.
std::vector<std::string_view> py_splitlines(std::string_view text);

} // namespace fv::detail
