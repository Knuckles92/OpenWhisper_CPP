#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "base/atomic_file.h"
#include "base/identity.h"
#include "base/testing/fake_clock.h"
#include "testkit/testkit.h"
#include "update_native/storage_actions.h"
#include "update_native/transaction_storage.h"

TEST_CASE("transaction storage pins roots and owns its exclusive data lease", "[update_native]") {
    fv::testkit::TempDataRoot temp;
    fv::update_native::JournalContext context{temp.path() / "data", temp.path() / "app" / "Fiori"};
    const auto paths =
        fv::update_native::journal_paths(context, "0123456789abcdef0123456789abcdef");
    std::filesystem::create_directories(paths.transaction_dir);
    std::filesystem::create_directories(paths.install_dir.parent_path());
    fv::update_native::TransactionStorage storage(context, "0123456789abcdef0123456789abcdef");
    const auto settings = paths.data_root / fv::identity().settings_file;
    fv::write_file_atomic(settings, "before");
    CHECK_THROWS(storage.snapshot_with_pinned_sources());
    storage.acquire_data_lease();
    CHECK(storage.has_data_lease());
    CHECK_THROWS(fv::DataLease::acquire(paths.data_root, fv::identity(), fv::LeaseMode::Shared));
    storage.snapshot_with_pinned_sources();
    fv::write_file_atomic(settings, "after");
    storage.restore_with_pinned_sources();
    std::ifstream input(settings);
    std::string restored;
    input >> restored;
    CHECK(restored == "before");
    storage.release_data_lease();
    CHECK_FALSE(storage.has_data_lease());
    CHECK_NOTHROW(fv::DataLease::acquire(paths.data_root, fv::identity(), fv::LeaseMode::Shared));
}

TEST_CASE("transaction storage actions swap and recover a temporary installation",
          "[update_native]") {
    fv::testkit::TempDataRoot temp;
    fv::update_native::JournalContext context{temp.path() / "data",
                                              temp.path() / "programs" / "app"};
    constexpr auto id = "0123456789abcdef0123456789abcdef";
    const auto paths = fv::update_native::journal_paths(context, id);
    std::filesystem::create_directories(paths.transaction_dir);
    std::filesystem::create_directories(paths.install_dir);
    std::filesystem::create_directories(paths.candidate_dir);
    fv::write_file_atomic(paths.install_dir / "old", "old");
    fv::write_file_atomic(paths.candidate_dir / "new", "new");
    fv::update_native::StorageActions actions(context, id);
    actions.move_old_to_rollback();
    actions.move_candidate_to_install();
    CHECK(std::filesystem::exists(paths.rollback_dir / "old"));
    CHECK(std::filesystem::exists(paths.install_dir / "new"));
    actions.move_new_to_failed();
    actions.move_rollback_to_install();
    CHECK(std::filesystem::exists(paths.install_dir / "old"));
}

namespace {
namespace fs = std::filesystem;
namespace u = fv::update_native;
constexpr auto kLockId = "0123456789abcdef0123456789abcdef";

// An installed tree, and something sitting where it has to be moved to.
struct BlockedMove {
    fv::testkit::TempDataRoot temp;
    u::JournalContext context{temp.path() / "data", temp.path() / "programs" / "app"};
    u::JournalPaths paths = u::journal_paths(context, kLockId);
    BlockedMove() {
        fs::create_directories(paths.transaction_dir);
        fs::create_directories(paths.install_dir);
        fs::create_directories(paths.rollback_dir);
        fv::write_file_atomic(paths.install_dir / "old", "old");
    }
};
} // namespace

// from
// tests/test_app_update_apply.py::TestLockedFiles::test_a_move_that_is_blocked_for_a_moment_still_happens
TEST_CASE("a directory move that is blocked for a moment still happens", "[update_native]") {
    BlockedMove f;
    u::StorageActions actions(f.context, kLockId);
    u::LockRetry retry;
    retry.timeout = std::chrono::seconds(30);
    // Stands in for the sharing violation of a scanner or of an image still mapped.
    retry.options.locked = [](const std::system_error&) {
        return true;
    };
    std::vector<std::chrono::milliseconds> waits;
    retry.options.wait = [&](std::chrono::milliseconds poll) {
        waits.push_back(poll);
        if (waits.size() == 2)
            fs::remove(f.paths.rollback_dir);
    };
    actions.set_lock_retry(retry);
    actions.move_old_to_rollback();
    // Refused twice, moved on the third attempt, a quarter of a second apart.
    CHECK(waits == std::vector<std::chrono::milliseconds>(2, std::chrono::milliseconds(250)));
    CHECK(fs::exists(f.paths.rollback_dir / "old"));
    CHECK_FALSE(fs::exists(f.paths.install_dir));
}

// from tests/test_app_update_apply.py::TestLockedFiles::test_a_lock_that_never_clears_still_fails
TEST_CASE("a lock that never clears still fails the move", "[update_native]") {
    BlockedMove f;
    u::StorageActions actions(f.context, kLockId);
    fv::FakeClock clock;
    u::LockRetry retry;
    retry.timeout = std::chrono::seconds(1);
    retry.options.clock = &clock;
    retry.options.locked = [](const std::system_error&) {
        return true;
    };
    int waits = 0;
    retry.options.wait = [&](std::chrono::milliseconds poll) {
        ++waits;
        clock.advance(poll);
    };
    actions.set_lock_retry(retry);
    CHECK_THROWS_AS(actions.move_old_to_rollback(), std::system_error);
    CHECK(waits == 4);
    CHECK(fs::exists(f.paths.install_dir / "old"));
    CHECK(fs::is_empty(f.paths.rollback_dir));
}

// from tests/test_app_update_apply.py::TestLockedFiles::test_an_ordinary_error_is_not_retried
TEST_CASE("an ordinary error is not retried", "[update_native]") {
    BlockedMove f;
    u::StorageActions actions(f.context, kLockId);
    u::LockRetry retry;
    int waits = 0;
    retry.options.wait = [&](std::chrono::milliseconds) {
        ++waits;
    };
    actions.set_lock_retry(retry);
    // Nothing to move: the candidate does not exist. That is no lock on any system.
    CHECK_THROWS_AS(actions.move_candidate_to_install(), std::system_error);
    // An occupied destination is not a lock either, so it is never waited out or replaced.
    CHECK_THROWS_AS(actions.move_old_to_rollback(), std::system_error);
    CHECK(waits == 0);
    CHECK(fs::exists(f.paths.install_dir / "old"));
}
