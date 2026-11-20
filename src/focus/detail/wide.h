#pragma once

#include <string>
#include <string_view>

// UTF-16 <-> UTF-8 for the Win32 and COM calls (Windows only; defined in win/).
namespace fv::focus::detail {

// Unpaired surrogates become U+FFFD.
std::string narrow(std::wstring_view wide);
std::wstring widen(std::string_view utf8);

} // namespace fv::focus::detail
