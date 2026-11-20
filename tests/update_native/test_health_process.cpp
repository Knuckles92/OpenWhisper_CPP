#include "base/atomic_file.h"
#include "base/cancel.h"
#include "download/verified_files.h"
#include "testkit/testkit.h"
#include "update_native/health_process.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

TEST_CASE("health launch rejects invalid state before starting a process", "[update_native]") {
    fv::testkit::TempDataRoot temp;
    constexpr auto id = "0123456789abcdef0123456789abcdef";
    fv::update_native::JournalContext context{temp.path() / "data",
                                              temp.path() / "programs" / "app"};
    const auto paths = fv::update_native::journal_paths(context, id);
    std::filesystem::create_directories(paths.transaction_dir);
    std::filesystem::create_directories(paths.install_dir);
    fv::update_native::TransactionStorage storage(context, id);
    fv::update_native::HealthProcess process;
    using namespace std::chrono_literals;
    const std::string hash(64, 'a');
    CHECK_THROWS(process.launch_and_wait(storage, "bad", hash, 1ms, {}));
    storage.acquire_data_lease();
    CHECK_THROWS(process.launch_and_wait(storage, id, hash, 1ms, {}));
    storage.release_data_lease();
    CHECK_THROWS(process.launch_and_wait(storage, id, hash, 1ms, {})); // no executable
}

#ifndef _WIN32
namespace {
namespace fs = std::filesystem;
// An installed "application" that is a script, with its transaction folder.
struct HealthFixture {
    fv::testkit::TempDataRoot temp;
    fv::update_native::JournalContext context{temp.path() / "data",
                                              temp.path() / "programs" / "app"};
    std::string id = "0123456789abcdef0123456789abcdef";
    fv::update_native::JournalPaths paths = fv::update_native::journal_paths(context, id);
    std::string hash;
    explicit HealthFixture(const std::string& script) {
        fs::create_directories(paths.transaction_dir);
        fs::create_directories(paths.install_dir);
        const auto app = paths.install_dir / "fiorivoce.exe";
        fv::write_file_atomic(app, script);
        fs::permissions(app, fs::perms::owner_exec, fs::perm_options::add);
        hash = fv::download::sha256_file(app, true);
    }
};
} // namespace

// from tests/test_update_resilience.py::test_health_process_exit_fails_without_waiting_for_timeout
TEST_CASE("a health process that exits fails without waiting for the timeout", "[update_native]") {
    using namespace std::chrono_literals;
    HealthFixture f("#!/bin/sh\nexit 1\n");
    fv::update_native::TransactionStorage storage(f.context, f.id);
    fv::update_native::HealthProcess process;
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(process.launch_and_wait(storage, f.id, f.hash, 30s, {}));
    CHECK(std::chrono::steady_clock::now() - started < 10s);
    CHECK_NOTHROW(process.stop());
    // One object, one process: it is never launched twice.
    CHECK_THROWS(process.launch_and_wait(storage, f.id, f.hash, 30s, {}));
}

TEST_CASE("a health process confirms with the whole token and is stopped only when it failed",
          "[update_native]") {
    using namespace std::chrono_literals;
    const std::string token(32, 'b');
    SECTION("the token it was given") {
        HealthFixture f("#!/bin/sh\ntx=\"$(dirname \"$0\")/../../data/updates/tx/$4\"\n"
                        "printf %s \"$2\" > \"$tx/h.part\" && mv \"$tx/h.part\" \"$tx/healthy\"\n"
                        "exec sleep 5\n");
        fv::update_native::TransactionStorage storage(f.context, f.id);
        fv::update_native::HealthProcess process;
        CHECK(process.launch_and_wait(storage, token, f.hash, 30s, {}));
    }
    SECTION("another token, until the wait is over") {
        HealthFixture f("#!/bin/sh\ntx=\"$(dirname \"$0\")/../../data/updates/tx/$4\"\n"
                        "printf %s wrong > \"$tx/h.part\" && mv \"$tx/h.part\" \"$tx/healthy\"\n"
                        "exec sleep 30\n");
        fv::update_native::TransactionStorage storage(f.context, f.id);
        fv::update_native::HealthProcess process;
        const auto started = std::chrono::steady_clock::now();
        CHECK_FALSE(process.launch_and_wait(storage, token, f.hash, 400ms, {}));
        CHECK(std::chrono::steady_clock::now() - started >= 400ms);
        // The failed version is ended before its tree is moved away.
        process.stop();
        CHECK(std::chrono::steady_clock::now() - started < 20s);
    }
    SECTION("a cancel ends the wait") {
        HealthFixture f("#!/bin/sh\nexec sleep 30\n");
        fv::update_native::TransactionStorage storage(f.context, f.id);
        fv::update_native::HealthProcess process;
        fv::CancelSource cancel;
        cancel.cancel();
        CHECK_FALSE(process.launch_and_wait(storage, token, f.hash, 30s, cancel.token()));
        process.stop();
    }
}
#else
// The same wait over the Windows child: hostname.exe rejects these arguments and exits at once.
TEST_CASE("a health process that exits fails without waiting for the timeout",
          "[update_native][platform]") {
    using namespace std::chrono_literals;
    namespace fs = std::filesystem;
    fv::testkit::TempDataRoot temp;
    const std::string id = "0123456789abcdef0123456789abcdef";
    fv::update_native::JournalContext context{temp.path() / "data",
                                              temp.path() / "programs" / "app"};
    const auto paths = fv::update_native::journal_paths(context, id);
    fs::create_directories(paths.transaction_dir);
    fs::create_directories(paths.install_dir);
    wchar_t system_dir[MAX_PATH]{};
    REQUIRE(GetSystemDirectoryW(system_dir, MAX_PATH) > 0);
    const auto app = paths.install_dir / "fiorivoce.exe";
    fs::copy_file(fs::path(system_dir) / "hostname.exe", app);
    fv::update_native::TransactionStorage storage(context, id);
    fv::update_native::HealthProcess process;
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(
        process.launch_and_wait(storage, id, fv::download::sha256_file(app, true), 30s, {}));
    CHECK(std::chrono::steady_clock::now() - started < 10s);
    CHECK_NOTHROW(process.stop());
}
#endif
