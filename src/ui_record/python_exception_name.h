#pragma once

#include <exception>
#include <string>

namespace fv {

// type(exc).__name__ for a failure the Upload tab's preview worker reports ("{product} couldn't
// read this file (RuntimeError)."): the Python class OpenWhisper would have named for the same
// failure, never a C++ type name. A pyport exception gives its own class; a file system error
// the OSError subclass its errno selects (PermissionError, FileNotFoundError, IsADirectoryError,
// NotADirectoryError, else OSError); std::bad_alloc MemoryError; std::invalid_argument and
// std::domain_error ValueError; std::out_of_range IndexError; std::overflow_error
// OverflowError; any other std::runtime_error RuntimeError; anything else the neutral
// "Exception".
std::string pythonExceptionName(const std::exception& error);

} // namespace fv
