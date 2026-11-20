// The class the Upload tab names when a preview fails for a reason it has no sentence for
// (ui_qt/widgets/upload_file_tab.py _start_previews: type(exc).__name__): the Python class
// OpenWhisper raised for the same failure, never a C++ type name.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <new>
#include <stdexcept>
#include <system_error>

#include "pyport/errors.h"
#include "ui_record/python_exception_name.h"

using fv::pythonExceptionName;

namespace {

std::filesystem::filesystem_error fileError(std::errc condition) {
    return std::filesystem::filesystem_error("file_size", std::filesystem::path("a.wav"),
                                             std::make_error_code(condition));
}

struct Unknown : std::exception {
    const char* what() const noexcept override { return "unknown"; }
};

} // namespace

TEST_CASE("a file system failure is named as Python's OSError for its errno", "[unit]") {
    // os.path.getsize on a file the user may not read, one gone, a folder and so on.
    CHECK(pythonExceptionName(fileError(std::errc::permission_denied)) == "PermissionError");
    CHECK(pythonExceptionName(fileError(std::errc::operation_not_permitted)) == "PermissionError");
    CHECK(pythonExceptionName(fileError(std::errc::no_such_file_or_directory)) ==
          "FileNotFoundError");
    CHECK(pythonExceptionName(fileError(std::errc::is_a_directory)) == "IsADirectoryError");
    CHECK(pythonExceptionName(fileError(std::errc::not_a_directory)) == "NotADirectoryError");
    CHECK(pythonExceptionName(fileError(std::errc::io_error)) == "OSError");
    CHECK(pythonExceptionName(std::system_error(std::make_error_code(std::errc::io_error))) ==
          "OSError");
}

TEST_CASE("a pyport exception keeps its Python class", "[unit]") {
    CHECK(pythonExceptionName(fv::py::ValueError("cannot convert float NaN to integer")) ==
          "ValueError");
    CHECK(pythonExceptionName(fv::py::OverflowError("cannot convert float infinity to integer")) ==
          "OverflowError");
}

TEST_CASE("standard exceptions are named as the Python classes they stand for", "[unit]") {
    CHECK(pythonExceptionName(std::runtime_error("decoder exploded")) == "RuntimeError");
    CHECK(pythonExceptionName(std::bad_alloc()) == "MemoryError");
    CHECK(pythonExceptionName(std::invalid_argument("x")) == "ValueError");
    CHECK(pythonExceptionName(std::domain_error("x")) == "ValueError");
    CHECK(pythonExceptionName(std::out_of_range("x")) == "IndexError");
    CHECK(pythonExceptionName(std::overflow_error("x")) == "OverflowError");
}

TEST_CASE("an exception with no Python counterpart is named neutrally", "[unit]") {
    CHECK(pythonExceptionName(std::logic_error("x")) == "Exception");
    CHECK(pythonExceptionName(std::length_error("x")) == "Exception");
    CHECK(pythonExceptionName(Unknown()) == "Exception");
}
