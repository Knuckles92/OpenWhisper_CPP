#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>

#include "update/install_probe.h"
#include "update_native/parent_process.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace u = fv::update_native;

TEST_CASE("parent process wait verifies current image and creation time", "[update_native]") {
#if !defined(_WIN32) && !defined(__linux__)
    SKIP("process identity is read on Windows and Linux only");
#endif
#ifdef _WIN32
    const std::uint32_t pid = GetCurrentProcessId();
#else
    const auto pid = static_cast<std::uint32_t>(::getpid());
#endif
    const auto ticks = static_cast<std::uint64_t>(u::current_process_creation_time());
    REQUIRE(ticks > 0);
    const auto exe = fv::update::running_app_executable();
    using namespace std::chrono_literals;
    // This process is running: the wait ends without it, and nothing is terminated.
    CHECK_FALSE(u::wait_for_parent(pid, exe, ticks, 0ms));
    // A recycled PID has another creation time or another image: never the process to wait for.
    CHECK_THROWS(u::wait_for_parent(pid, exe, ticks + 1, 0ms));
    CHECK_THROWS(u::wait_for_parent(pid, exe.parent_path() / "different.exe", ticks, 0ms));
    // Without a recorded image and creation time there is no identity to match.
    CHECK_THROWS(u::wait_for_parent(pid, {}, ticks, 0ms));
    CHECK_THROWS(u::wait_for_parent(pid, exe, 0, 0ms));
    CHECK_THROWS(u::wait_for_parent(0, exe, ticks, 0ms));
}
