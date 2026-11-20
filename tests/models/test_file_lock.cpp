// FileLock, the cross-process lock the speech-model cache takes around a download: who holds it,
// and whether its file outlives the last holder (filelock removes it on Windows only).

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <thread>

#include "models/detail/fs_native.h"
#include "models_test_support.h"

using fv::models::detail::FileLock;
using fv::models::test::TempDir;
using namespace std::chrono_literals;

namespace {

#if defined(_WIN32)
constexpr bool kLastHolderRemovesTheFile = true;
#else
constexpr bool kLastHolderRemovesTheFile = false;
#endif

} // namespace

TEST_CASE("a released lock leaves its file only where filelock does", "[file_lock]") {
    const TempDir temp;
    const std::filesystem::path file = temp.path() / "parakeet.lock";
    {
        const FileLock lock(file);
        CHECK(std::filesystem::exists(file));
    }
    CHECK(std::filesystem::exists(file) != kLastHolderRemovesTheFile);
    // Taken again, whether or not the file is still there.
    {
        const FileLock again(file);
        CHECK(std::filesystem::exists(file));
    }
    CHECK(std::filesystem::exists(file) != kLastHolderRemovesTheFile);
}

TEST_CASE("a waiter takes the lock when it is released and the last holder removes the file",
          "[file_lock]") {
    const TempDir temp;
    const std::filesystem::path file = temp.path() / "parakeet.lock";
    std::atomic<bool> holding{false};
    std::atomic<bool> waiter_has_it{false};
    std::atomic<bool> overlapped{false};
    std::atomic<bool> file_while_waiter_holds{false};
    std::optional<FileLock> first(std::in_place, file);
    holding = true;

    std::thread waiter([&] {
        const FileLock second(file);
        overlapped = holding.load();
        waiter_has_it = true;
        file_while_waiter_holds = std::filesystem::exists(file);
        std::this_thread::sleep_for(50ms);
    });
    // Long enough for the waiter to be blocked on the lock, not still opening the file.
    std::this_thread::sleep_for(200ms);
    CHECK_FALSE(waiter_has_it.load());
    holding = false;
    first.reset();
    waiter.join();

    CHECK(waiter_has_it.load());
    CHECK_FALSE(overlapped.load());
    // The first holder's removal failed while the waiter had the file open.
    CHECK(file_while_waiter_holds.load());
    CHECK(std::filesystem::exists(file) != kLastHolderRemovesTheFile);
}
