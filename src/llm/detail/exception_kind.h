#pragma once

#include <exception>
#include <string_view>

#include "pyport/errors.h"

namespace fv::llm::detail {

// The Python class of an exception, for a log line that must not carry its text.
inline std::string_view exception_kind(const std::exception& error) {
    if (const auto* python = dynamic_cast<const py::PyError*>(&error)) {
        return python->type_name();
    }
    return "Exception";
}

} // namespace fv::llm::detail
