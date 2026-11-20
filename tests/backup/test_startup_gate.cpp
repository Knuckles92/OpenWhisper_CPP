// The startup restore gate and the restart after a restore (OpenWhisper's
// tests/test_backup_startup.py; the data lease itself is tests/base/test_data_lease.cpp).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "backup/create.h"
#include "backup/restart.h"
#include "backup/restore.h"
#include "backup/startup_gate.h"
#include "backup/testing/sample_data.h"
#include "base/data_lease.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "test_support.h"
#include "testkit/testkit.h"

namespace fs = std::filesystem;
using Catch::Matchers::Message;
using fv::DataLease;
using fv::DataLeaseBusy;
using fv::identity;
using fv::LeaseMode;
using fv::StartupRestoreError;
using fv::backup::acquire_startup_data_lease;
using fv::backup::release_startup_data_lease;
using fv::backup::StartupRestoreSteps;
using fv::test::ChildProcess;

namespace {

// _reader_process: another process holding the shared lease until its stdin closes.
std::unique_ptr<ChildProcess> reader_process(const fs::path& data_dir) {
    auto process =
        std::make_unique<ChildProcess>(fv::path_from_utf8(FV_BACKUP_LEASE_HELPER),
                                       std::vector<std::string>{fv::path_to_utf8(data_dir)});
    REQUIRE(process->read_line(std::chrono::seconds(10)) == std::optional<std::string>("READY"));
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

// The gate's steps over a marker file, as the Python test monkeypatches them.
struct MarkerSteps {
    fs::path marker;
    std::vector<fs::path> applied;
    bool apply_result = true;

    StartupRestoreSteps steps() {
        StartupRestoreSteps result;
        result.pending = [this](const fs::path&) {
            return fs::exists(marker);
        };
        result.apply = [this](const fs::path& root) {
            applied.push_back(root);
            if (apply_result) {
                fs::remove(marker);
            }
            return apply_result;
        };
        return result;
    }
};

struct Released {
    Released() { release_startup_data_lease(); }
    ~Released() { release_startup_data_lease(); }
};

} // namespace

TEST_CASE("pending restore never runs while another process uses data",
          "[test_backup_startup][platform]") {
    // from
    // tests/test_backup_startup.py::test_pending_restore_never_runs_while_another_process_uses_data
    const Released released;
    fv::testkit::TempDataRoot temp;
    MarkerSteps restore{temp.path() / "pending-test"};
    fv::backup::testing::write_bytes(restore.marker, "pending");
    {
        auto other = reader_process(temp.path());
        CHECK_THROWS_MATCHES(
            acquire_startup_data_lease(temp.path(), identity(), restore.steps()),
            StartupRestoreError,
            Message(
                "A backup restore is pending, but another OpenWhisper_CPP process is still using "
                "this data. Close every OpenWhisper_CPP window and History API process, then "
                "launch again to finish the restore."));
        CHECK(fs::exists(restore.marker));
        CHECK(restore.applied.empty());
        stop(*other);
    }

    DataLease lease = acquire_startup_data_lease(temp.path(), identity(), restore.steps());
    CHECK(lease.held());
    CHECK(lease.mode() == LeaseMode::Shared);
    CHECK(restore.applied == std::vector<fs::path>{fv::normalize_data_root(temp.path())});
    CHECK_THROWS_AS(DataLease::acquire(temp.path(), identity(), LeaseMode::Exclusive),
                    DataLeaseBusy);
}

TEST_CASE("normal startup holds the shared lease without a restore", "[backup]") {
    const Released released;
    fv::testkit::TempDataRoot temp;
    StartupRestoreSteps steps;
    int applies = 0;
    steps.pending = [](const fs::path&) {
        return false;
    };
    steps.apply = [&](const fs::path&) {
        ++applies;
        return true;
    };
    DataLease lease = acquire_startup_data_lease(temp.path(), identity(), steps);
    CHECK(applies == 0);
    CHECK(lease.mode() == LeaseMode::Shared);
    CHECK(lease.root() == fv::normalize_data_root(temp.path()));
    CHECK_NOTHROW(DataLease::acquire(temp.path(), identity(), LeaseMode::Shared));
    CHECK_THROWS_AS(DataLease::acquire(temp.path(), identity(), LeaseMode::Exclusive),
                    DataLeaseBusy);
}

TEST_CASE("the gate's refusals", "[backup]") {
    const Released released;
    fv::testkit::TempDataRoot temp;
    MarkerSteps restore{temp.path() / "pending-test"};
    fv::backup::testing::write_bytes(restore.marker, "pending");

    SECTION("another process restoring holds the data exclusively") {
        DataLease restoring = DataLease::acquire(temp.path(), identity(), LeaseMode::Exclusive);
        CHECK_THROWS_MATCHES(acquire_startup_data_lease(temp.path(), identity(), restore.steps()),
                             StartupRestoreError,
                             Message("Another OpenWhisper_CPP process is restoring this data. Wait "
                                     "for it to finish, then launch again."));
    }
    SECTION("a restore is pending while another user shares the data") {
        DataLease user = DataLease::acquire(temp.path(), identity(), LeaseMode::Shared);
        CHECK_THROWS_MATCHES(
            acquire_startup_data_lease(temp.path(), identity(), restore.steps()),
            StartupRestoreError,
            Message(
                "A backup restore is pending, but another OpenWhisper_CPP process is still using "
                "this data. Close every OpenWhisper_CPP window and History API process, then "
                "launch again to finish the restore."));
    }
    SECTION("the restore was not applied") {
        restore.apply_result = false;
        CHECK_THROWS_MATCHES(
            acquire_startup_data_lease(temp.path(), identity(), restore.steps()),
            StartupRestoreError,
            Message("The pending backup restore was not applied. The existing data was left in "
                    "place; launch again after resolving the restore error."));
        CHECK(restore.applied.size() == 1);
    }
    SECTION("a restore that keeps coming back") {
        StartupRestoreSteps steps;
        int applies = 0;
        steps.pending = [](const fs::path&) {
            return true;
        };
        steps.apply = [&](const fs::path&) {
            ++applies;
            return true;
        };
        CHECK_THROWS_MATCHES(acquire_startup_data_lease(temp.path(), identity(), steps),
                             StartupRestoreError,
                             Message("The backup restore changed repeatedly during startup; "
                                     "launch again."));
        CHECK(applies == 4);
    }
    // Every refusal leaves the data free.
    CHECK_NOTHROW(DataLease::acquire(temp.path(), identity(), LeaseMode::Exclusive));
}

TEST_CASE("the gate admits one data root per process", "[backup]") {
    const Released released;
    fv::testkit::TempDataRoot temp;
    StartupRestoreSteps steps;
    int checks = 0;
    steps.pending = [&](const fs::path&) {
        ++checks;
        return false;
    };
    steps.apply = [](const fs::path&) {
        return false;
    };
    DataLease first = acquire_startup_data_lease(temp.path() / "one", identity(), steps);
    DataLease again = acquire_startup_data_lease(temp.path() / "one" / ".", identity(), steps);
    CHECK(again.held());
    CHECK(again.mode() == LeaseMode::Shared);
    CHECK(checks == 1);
    CHECK_THROWS_MATCHES(acquire_startup_data_lease(temp.path() / "two", identity(), steps),
                         StartupRestoreError,
                         Message("OpenWhisper_CPP already holds a different data directory."));
    release_startup_data_lease();
    CHECK_NOTHROW(acquire_startup_data_lease(temp.path() / "two", identity(), steps));
}

TEST_CASE("the gate applies a staged restore before handing out the shared lease", "[backup]") {
    const Released released;
    fv::testkit::TempDataRoot temp;
    // Resolved, as the coordinator resolves the root it stages into: the gate's root is
    // realpath'd, and a short (8.3) temporary path would not lie below it.
    const fs::path base = fs::weakly_canonical(temp.path());
    const fs::path source = base / "source";
    const fs::path target = base / "target";
    fv::backup::testing::make_sample_data(source);
    fv::backup::testing::make_sample_data(target);
    fv::backup::testing::write_bytes(target / "recordings" / "prior-only.wav", "prior");
    fv::backup::CreateOptions options;
    options.data_root = source;
    options.app_version = "0.1.0";
    const fs::path archive = temp.path() / "portable.fvbackup";
    fv::backup::create_backup(archive, options);
    fv::backup::prepare_restore(archive, target);

    DataLease lease = acquire_startup_data_lease(target, identity());
    CHECK(lease.mode() == LeaseMode::Shared);
    CHECK_FALSE(fv::backup::pending_restore_exists(target));
    CHECK_FALSE(fs::exists(target / "recordings" / "prior-only.wav"));
    const auto previous = fv::backup::previous_data_dir(target);
    REQUIRE(previous.has_value());
    CHECK(fs::exists(*previous / "recordings" / "prior-only.wav"));
}

TEST_CASE("restart command drops update and restore args", "[test_backup_startup]") {
    // from tests/test_backup_startup.py::test_restart_command_drops_update_and_restore_args
    // (OpenWhisper_CPP always runs an installed or built executable: the frozen branch)
    const fs::path exe = fs::absolute("openwhisper-cpp.exe");
    CHECK(fv::backup::restart_command(exe) == std::vector<std::string>{fv::path_to_utf8(exe)});
}

TEST_CASE("spawn_restart starts the executable alone and passes a failure on", "[backup]") {
    struct Recording final : fv::backup::ProcessStarter {
        std::vector<std::pair<fs::path, std::vector<std::string>>> starts;
        std::optional<std::string> failure;
        std::expected<void, std::string> start(const fs::path& exe,
                                               std::span<const std::string> args) override {
            starts.emplace_back(exe, std::vector<std::string>(args.begin(), args.end()));
            if (failure) {
                return std::unexpected(*failure);
            }
            return {};
        }
    } starter;
    const fs::path exe = fs::absolute("OpenWhisper_CPP") / "openwhisper-cpp.exe";
    CHECK(fv::backup::spawn_restart(exe, starter).has_value());
    REQUIRE(starter.starts.size() == 1);
    CHECK(starter.starts[0].first == exe);
    CHECK(starter.starts[0].second.empty());
    CHECK(fv::backup::restart_exit_code == 73);

    starter.failure = "The system cannot find the file specified.";
    const auto result = fv::backup::spawn_restart(exe, starter);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == "The system cannot find the file specified.");
}
