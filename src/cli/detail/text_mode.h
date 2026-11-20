#pragma once

#include <string_view>

namespace fv::cli::detail {

// What Python's text mode writes for "\n": os.linesep ("\r\n" on Windows).
std::string_view text_mode_newline();

} // namespace fv::cli::detail
