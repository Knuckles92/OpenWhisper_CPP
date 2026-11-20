// The data lease across processes (OpenWhisper's services/backup_startup.py DataLease).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <fstream>
#include <iterator>
#include <thread>

#include "base/data_lease.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "test_support.h"

using fv::DataLease;
using fv::DataLeaseBusy;
using fv::identity;
using fv::LeaseMode;
using fv::test::ChildProcess;
using fv::test::TempDir;

namespace {

// tests/test_backup_startup.py _reader_process: holds a shared lease until its stdin closes.
std::unique_ptr<ChildProcess> reader_process(const std::filesystem::path& data_dir) {
    auto process = std::make_unique<ChildProcess>(
        fv::path_from_utf8(FV_LEASE_HELPER), std::vector<std::string>{fv::path_to_utf8(data_dir)});
    const auto ready = process->read_line(std::chrono::seconds(10));
    REQUIRE(ready == std::optional<std::string>("READY"));
    return process;
}

// _stop: closing stdin lets the helper unlock the file itself; killing it would leave the unlock
// to Windows, which releases a dead process's locks some time after it exits.
void stop(ChildProcess& process) {
    process.close_stdin();
    if (!process.wait(std::chrono::seconds(10))) {
        process.kill();
    }
    REQUIRE(process.wait(std::chrono::seconds(10)));
}

} // namespace

TEST_CASE("shared processes coexist and exclusive waits for exit", "[test_backup_startup]") {
    // from tests/test_backup_startup.py::test_shared_processes_coexist_and_exclusive_waits_for_exit
    TempDir temp;
    auto other = reader_process(temp.path());
    {
        auto shared = DataLease::acquire(temp.path(), identity(), LeaseMode::Shared);
        CHECK(shared.held());
        CHECK_THROWS_MATCHES(
            DataLease::acquire(temp.path(), identity(), LeaseMode::Exclusive), DataLeaseBusy,
            Catch::Matchers::Message(
                "The application data is in use by another OpenWhisper_CPP process."));
    }
    stop(*other);

    // Windows may finish releasing a terminated process's region a moment after the wait
    // reports its exit.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    DataLease exclusive;
    for (;;) {
        try {
            exclusive = DataLease::acquire(temp.path(), identity(), LeaseMode::Exclusive);
            break;
        } catch (const DataLeaseBusy&) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    CHECK(exclusive.held());
    CHECK_THROWS_AS(DataLease::acquire(temp.path(), identity(), LeaseMode::Shared), DataLeaseBusy);
}

TEST_CASE("two shared leases coexist in one process and release lets an exclusive one in") {
    TempDir temp;
    auto first = DataLease::acquire(temp.path(), identity(), LeaseMode::Shared);
    auto second = DataLease::acquire(temp.path(), identity(), LeaseMode::Shared);
    CHECK_THROWS_AS(DataLease::acquire(temp.path(), identity(), LeaseMode::Exclusive),
                    DataLeaseBusy);

    first.release();
    CHECK_FALSE(first.held());
    CHECK_THROWS_AS(DataLease::acquire(temp.path(), identity(), LeaseMode::Exclusive),
                    DataLeaseBusy);
    {
        DataLease moved = std::move(second);
        CHECK(moved.held());
        CHECK_FALSE(second.held());
    } // released by the destructor

    auto exclusive = DataLease::acquire(temp.path(), identity(), LeaseMode::Exclusive);
    CHECK(exclusive.mode() == LeaseMode::Exclusive);
}

TEST_CASE("the lease creates the data root and a one byte lock file") {
    TempDir temp;
    const auto root = temp.path() / "nested" / "data";
    auto lease = DataLease::acquire(root, identity(), LeaseMode::Shared);

    const auto lock = root / ".openwhisper-cpp-data.lock";
    REQUIRE(std::filesystem::is_regular_file(lock));
    CHECK(std::filesystem::file_size(lock) == 1);
    lease.release();
    std::ifstream file(lock, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    CHECK(bytes == std::string(1, '\0'));
}

TEST_CASE("the lease keeps an existing lock file's bytes") {
    TempDir temp;
    std::ofstream(temp.path() / ".openwhisper-cpp-data.lock", std::ios::binary) << "xyz";
    DataLease::acquire(temp.path(), identity(), LeaseMode::Exclusive).release();
    CHECK(std::filesystem::file_size(temp.path() / ".openwhisper-cpp-data.lock") == 3);
}

TEST_CASE("a lock that is a folder cannot be opened") {
    // Python's os.open raises an OSError here (PermissionError on Windows, IsADirectoryError on
    // Linux), not the "not a file" StartupRestoreError, which a FIFO or device would get.
    TempDir temp;
    std::filesystem::create_directories(temp.path() / ".openwhisper-cpp-data.lock");
    CHECK_THROWS_AS(DataLease::acquire(temp.path(), identity(), LeaseMode::Shared),
                    std::filesystem::filesystem_error);
}

TEST_CASE("a lock that is a symbolic link is refused") {
    TempDir temp;
    std::ofstream(temp.path() / "target", std::ios::binary) << "x";
    std::error_code error;
    std::filesystem::create_symlink(temp.path() / "target",
                                    temp.path() / ".openwhisper-cpp-data.lock", error);
    if (error) {
        SKIP("creating a symbolic link needs Developer Mode or elevation here");
    }
    CHECK_THROWS_MATCHES(DataLease::acquire(temp.path(), identity(), LeaseMode::Shared),
                         fv::StartupRestoreError,
                         Catch::Matchers::Message("The application data lock is a symbolic link."));
}

TEST_CASE("the lease names its root normalized") {
    TempDir temp;
    const auto spelled = temp.path() / "data" / ".." / "data";
    auto lease = DataLease::acquire(spelled, identity(), LeaseMode::Shared);
    CHECK(lease.root() == fv::normalize_data_root(temp.path() / "data"));
#if defined(_WIN32)
    const std::string text = fv::path_to_utf8(lease.root());
    CHECK(text.find('/') == std::string::npos);
    for (const char c : text) {
        CHECK_FALSE((c >= 'A' && c <= 'Z'));
    }
#endif
}

TEST_CASE("the busy message names the product of the identity it was asked for") {
    TempDir temp;
    auto held = DataLease::acquire(temp.path(), fv::legacy_identity(), LeaseMode::Exclusive);
    CHECK(std::filesystem::exists(temp.path() / ".openwhisper-data.lock"));
    CHECK_THROWS_MATCHES(
        DataLease::acquire(temp.path(), fv::legacy_identity(), LeaseMode::Shared), DataLeaseBusy,
        Catch::Matchers::Message("The application data is in use by another OpenWhisper process."));
    // OpenWhisper_CPP's own lease is a different file, so the two apps never block each other by
    // it.
    CHECK_NOTHROW(DataLease::acquire(temp.path(), identity(), LeaseMode::Exclusive));
}
