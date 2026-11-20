// No child gets a console window (A01-19): OpenWhisper started its helpers with
// CREATE_NO_WINDOW, so a console program never flashes a black window over the app. Every child
// starts through the launcher or start_detached; here a real console program started through
// each finds no console window of its own. Without the flag it would share the test runner's
// console or, from a runner without one, get a new console with a window, so either way it would
// report "window".
//
// Windows only: off Windows the launcher starts nothing and there are no console windows.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "process/detached.h"
#include "process/launcher.h"
#include "process_test_support.h"
#include "testkit/testkit.h"

#ifdef _WIN32

using fv::process_test::child;
using fv::process_test::read_file;
using fv::process_test::wait_until;

TEST_CASE("a program the launcher starts has no console window", "[process][platform]") {
    fv::testkit::TempDataRoot root;
    const auto report = root.path() / "console.txt";

    fv::ProcessSpec spec;
    spec.program = fv::path_from_utf8(child());
    spec.args = {"console-window", fv::path_to_utf8(report)};
    const auto started = fv::make_process_launcher()->start(spec);
    REQUIRE(started.has_value());
    const auto code = (*started)->wait(std::chrono::seconds(20));
    REQUIRE(code.has_value());
    CHECK(*code == 0);
    CHECK(read_file(report) == "none");
}

TEST_CASE("a program the launcher starts with pipes has no console window", "[process][platform]") {
    fv::testkit::TempDataRoot root;
    const auto report = root.path() / "console.txt";

    fv::ProcessSpec spec;
    spec.program = fv::path_from_utf8(child());
    spec.args = {"console-window", fv::path_to_utf8(report)};
    spec.pipes = true;
    const auto started = fv::make_process_launcher()->start(spec);
    REQUIRE(started.has_value());
    (*started)->on_bytes(nullptr, nullptr); // read and dropped
    (*started)->close_stdin();
    const auto code = (*started)->wait(std::chrono::seconds(20));
    REQUIRE(code.has_value());
    CHECK(*code == 0);
    CHECK(read_file(report) == "none");
}

TEST_CASE("a program start_detached starts has no console window", "[process][platform]") {
    fv::testkit::TempDataRoot root;
    const auto report = root.path() / "console.txt";

    const std::vector<std::string> args{"console-window", fv::path_to_utf8(report)};
    const auto started = fv::start_detached(fv::path_from_utf8(child()), args);
    REQUIRE(started.has_value());
    // The child writes its report through a rename, so a file that is there is whole.
    REQUIRE(wait_until([&] { return std::filesystem::exists(report); }));
    CHECK(read_file(report) == "none");
}

#endif
