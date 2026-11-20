#pragma once

#include <exception>
#include <stdexcept>
#include <string_view>

namespace fv::credentials_detail {

// A stored blob that is neither UTF-16 nor UTF-8 (keyring raises UnicodeDecodeError there).
class BlobDecodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The short class name that error copy and log lines carry in place of Python's
// type(exc).__name__. Never the message, which may hold what the backend was given.
std::string_view exception_type_name(const std::exception&);

} // namespace fv::credentials_detail
