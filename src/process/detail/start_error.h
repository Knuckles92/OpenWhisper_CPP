#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace fv::process_detail {

// The OSError subclass Python raises for a Windows error code (PC/errmap.h's winerror_to_errno,
// then the errno's exception class): "FileNotFoundError", "PermissionError", ... or "OSError".
std::string_view python_error_type(std::uint32_t winerror);

// programs.py _start_error: why a program couldn't start, as the user reads it.
std::string start_error(std::uint32_t winerror);

} // namespace fv::process_detail
