// What the next start collects from the updates folder: OpenWhisper's tests/test_app_update.py
// (TestPruneStaleDownloads), plus a file a running process really holds open.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "download_support.h"
#include "testkit/testkit.h"
#include "update/leftovers.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace update = fv::update;
using namespace fv::update::test;

namespace {

// os.utime(path, (0, 0)): modified at the Unix epoch.
void make_old(const std::filesystem::path& path) {
    const auto epoch = std::chrono::system_clock::time_point{};
#ifdef _WIN32
    std::filesystem::last_write_time(path, std::chrono::clock_cast<std::chrono::file_clock>(epoch));
#else
    std::filesystem::last_write_time(path, std::chrono::file_clock::from_sys(epoch));
#endif
}

std::chrono::system_clock::time_point now() {
    return std::chrono::system_clock::now();
}

} // namespace

// from tests/test_app_update.py::TestPruneStaleDownloads::test_a_finished_download_is_collected
TEST_CASE("a finished download is collected", "[test_app_update]") {
    fv::testkit::TempDataRoot root;
    const auto leftover = root.path() / "FioriVoce-Setup-2.4.2.exe";
    write_bytes(leftover, "installer");
    make_old(leftover);
    CHECK(update::prune_stale_downloads(root.path(), now()) ==
          std::vector<std::string>{"FioriVoce-Setup-2.4.2.exe"});
    CHECK_FALSE(std::filesystem::exists(leftover));
}

// from
// tests/test_app_update.py::TestPruneStaleDownloads::test_the_transaction_directory_is_left_to_its_owner
TEST_CASE("the transaction directory is left to its owner", "[test_app_update]") {
    fv::testkit::TempDataRoot root;
    const auto tx = root.path() / "tx" / std::string(32, 'a');
    std::filesystem::create_directories(tx);
    write_bytes(tx / "journal.json", "{}");
    make_old(root.path() / "tx");
    CHECK(update::prune_stale_downloads(root.path(), now()).empty());
    CHECK(std::filesystem::exists(tx / "journal.json"));
}

// from tests/test_app_update.py::TestPruneStaleDownloads::test_a_download_in_progress_is_spared
TEST_CASE("a download in progress is spared", "[test_app_update]") {
    fv::testkit::TempDataRoot root;
    const auto active = root.path() / "FioriVoce-2.4.3-win64.zip";
    write_bytes(active, "partial");
    CHECK(update::prune_stale_downloads(root.path(), now()).empty());
    CHECK(std::filesystem::exists(active));

    // Thirty seconds later it is fair game.
    CHECK(update::prune_stale_downloads(root.path(), now() + std::chrono::seconds(31)) ==
          std::vector<std::string>{"FioriVoce-2.4.3-win64.zip"});
}

// from
// tests/test_app_update.py::TestPruneStaleDownloads::test_a_locked_file_does_not_stop_the_others
TEST_CASE("a locked file does not stop the others", "[test_app_update]") {
    fv::testkit::TempDataRoot root;
    for (const auto* name : {"locked.exe", "collectable.exe"}) {
        write_bytes(root.path() / name, "x");
        make_old(root.path() / name);
    }
    const auto refuse_locked = [](const std::filesystem::path& path) {
        if (path.filename() == "locked.exe") {
            throw std::filesystem::filesystem_error(
                "in use", path, std::make_error_code(std::errc::permission_denied));
        }
        std::filesystem::remove(path);
    };
    CHECK(update::prune_stale_downloads(root.path(), now(), refuse_locked) ==
          std::vector<std::string>{"collectable.exe"});
    CHECK(std::filesystem::exists(root.path() / "locked.exe"));
}

TEST_CASE("a missing updates folder is created and yields nothing", "[update]") {
    fv::testkit::TempDataRoot root;
    const auto updates = root.path() / "updates";
    CHECK(update::prune_stale_downloads(updates, now()).empty());
    CHECK(std::filesystem::is_directory(updates));
}

#ifdef _WIN32
TEST_CASE("a setup still running keeps its file and the next start collects it",
          "[update][platform]") {
    fv::testkit::TempDataRoot root;
    const auto running = root.path() / "FioriVoce-Setup-0.1.1.exe";
    write_bytes(running, "setup");
    make_old(running);
    write_bytes(root.path() / "setup.log", "log");
    make_old(root.path() / "setup.log");
    // A running image is mapped without delete sharing; a handle that does not share delete
    // stands in for it.
    const HANDLE held = CreateFileW(running.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    const auto removed = update::prune_stale_downloads(root.path(), now());
    CloseHandle(held);
    CHECK(removed == std::vector<std::string>{"setup.log"});
    CHECK(std::filesystem::exists(running));
    CHECK(update::prune_stale_downloads(root.path(), now()) ==
          std::vector<std::string>{"FioriVoce-Setup-0.1.1.exe"});
}
#endif
