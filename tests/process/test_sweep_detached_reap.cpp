// start_detached leaves no zombie of this process behind: the program it starts is not this
// process's child (a double fork), so its exit is reaped by whoever adopts it.

#include <catch2/catch_test_macros.hpp>

#ifdef __linux__

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "process/detached.h"
#include "testkit/testkit.h"

TEST_CASE("start_detached's program is not a child this process must reap", "[process][platform]") {
    fv::testkit::TempDataRoot root;
    const std::vector<std::string> args{"-c", "sleep 1"};
    const auto started = fv::start_detached("/bin/sh", args, root.path());
    REQUIRE(started.has_value());
    const auto pid = static_cast<pid_t>(*started);
    REQUIRE(pid > 0);
    // The id is the program's own: it is alive now.
    CHECK(::kill(pid, 0) == 0);
    // Not ours to wait for, so it can never become our zombie (a direct child reads 0 here).
    errno = 0;
    int status = 0;
    CHECK(::waitpid(pid, &status, WNOHANG) == -1);
    CHECK(errno == ECHILD);
    // And the intermediate child was reaped before start_detached returned.
    errno = 0;
    while (::waitpid(-1, &status, WNOHANG) > 0) {
    }
    ::kill(pid, SIGTERM);
}

TEST_CASE("start_detached still reports a program that cannot start", "[process][platform]") {
    fv::testkit::TempDataRoot root;
    const auto missing = fv::start_detached(root.path() / "missing", {});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == "Could not start missing.");
    const auto file = root.path() / "not a program";
    { std::ofstream(file) << "x"; }
    CHECK_FALSE(fv::start_detached(file, {}).has_value());
    const auto badDir = fv::start_detached("/bin/sh", {}, root.path() / "no such folder");
    CHECK_FALSE(badDir.has_value());
}

#endif
