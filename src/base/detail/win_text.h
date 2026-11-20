#pragma once

#include <string>
#include <string_view>

// UTF-8 to and from the UTF-16 of the Windows API. Defined in win/ only.
namespace fv::detail {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view utf16);

} // namespace fv::detail
