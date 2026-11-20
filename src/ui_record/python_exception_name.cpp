#include "ui_record/python_exception_name.h"

#include <new>
#include <stdexcept>
#include <system_error>

#include "pyport/errors.h"

namespace fv {

std::string pythonExceptionName(const std::exception& error) {
    if (const auto* python = dynamic_cast<const py::PyError*>(&error)) {
        return std::string(python->type_name());
    }
    // filesystem_error and ios_base::failure too. The comparisons are by error condition, so a
    // Windows error code (ERROR_ACCESS_DENIED) matches as Python maps winerror to errno.
    if (const auto* system = dynamic_cast<const std::system_error*>(&error)) {
        const std::error_code code = system->code();
        if (code == std::errc::permission_denied || code == std::errc::operation_not_permitted) {
            return "PermissionError";
        }
        if (code == std::errc::no_such_file_or_directory) {
            return "FileNotFoundError";
        }
        if (code == std::errc::is_a_directory) {
            return "IsADirectoryError";
        }
        if (code == std::errc::not_a_directory) {
            return "NotADirectoryError";
        }
        return "OSError";
    }
    if (dynamic_cast<const std::bad_alloc*>(&error) != nullptr) {
        return "MemoryError";
    }
    if (dynamic_cast<const std::invalid_argument*>(&error) != nullptr ||
        dynamic_cast<const std::domain_error*>(&error) != nullptr) {
        return "ValueError";
    }
    if (dynamic_cast<const std::out_of_range*>(&error) != nullptr) {
        return "IndexError";
    }
    if (dynamic_cast<const std::overflow_error*>(&error) != nullptr) {
        return "OverflowError";
    }
    if (dynamic_cast<const std::runtime_error*>(&error) != nullptr) {
        return "RuntimeError";
    }
    return "Exception";
}

} // namespace fv
