#include "process/detail/start_error.h"

#include <format>

#include "process/run_once.h"

namespace fv::process_detail {

std::string_view python_error_type(std::uint32_t winerror) {
    switch (winerror) {
    case 2:   // ERROR_FILE_NOT_FOUND
    case 3:   // ERROR_PATH_NOT_FOUND
    case 15:  // ERROR_INVALID_DRIVE
    case 18:  // ERROR_NO_MORE_FILES
    case 53:  // ERROR_BAD_NETPATH
    case 67:  // ERROR_BAD_NET_NAME
    case 161: // ERROR_BAD_PATHNAME
    case 206: // ERROR_FILENAME_EXCED_RANGE
        return "FileNotFoundError";
    case 5:   // ERROR_ACCESS_DENIED
    case 16:  // ERROR_CURRENT_DIRECTORY
    case 65:  // ERROR_NETWORK_ACCESS_DENIED
    case 82:  // ERROR_CANNOT_MAKE
    case 83:  // ERROR_FAIL_I24
    case 108: // ERROR_DRIVE_LOCKED
    case 132: // ERROR_SEEK_ON_DEVICE
    case 158: // ERROR_NOT_LOCKED
    case 167: // ERROR_LOCK_FAILED
        return "PermissionError";
    case 80:  // ERROR_FILE_EXISTS
    case 183: // ERROR_ALREADY_EXISTS
        return "FileExistsError";
    case 267: // ERROR_DIRECTORY
        return "NotADirectoryError";
    case 128: // ERROR_WAIT_NO_CHILDREN
    case 129: // ERROR_CHILD_NOT_COMPLETE
        return "ChildProcessError";
    case 89:  // ERROR_NO_PROC_SLOTS
    case 164: // ERROR_MAX_THRDS_REACHED
    case 215: // ERROR_NESTING_NOT_ALLOWED
        return "BlockingIOError";
    case 109: // ERROR_BROKEN_PIPE
    case 232: // ERROR_NO_DATA
        return "BrokenPipeError";
    default:
        // ERROR_WRITE_PROTECT (19) to ERROR_SHARING_BUFFER_EXCEEDED (36) are EACCES too.
        if (winerror >= 19 && winerror <= 36) {
            return "PermissionError";
        }
        return "OSError";
    }
}

std::string start_error(std::uint32_t winerror) {
    const auto type = python_error_type(winerror);
    if (type == "FileNotFoundError") {
        return std::string(PROGRAM_NOT_FOUND);
    }
    if (type == "PermissionError") {
        return std::string(PROGRAM_NOT_ALLOWED);
    }
    if (winerror == 193) { // ERROR_BAD_EXE_FORMAT
        return std::string(PROGRAM_NOT_RUNNABLE);
    }
    return std::format("The program couldn't start ({} {}).", type, winerror);
}

} // namespace fv::process_detail
