// What a normal start does with the transactions it finds (startup_recovery.h): collect the
// finished ones, abandon a preparation whose application is gone, and leave everything it cannot
// account for. From OpenWhisper's TestAbandonedTransactions (tests/test_app_update_apply.py) and
// tests/test_update_resilience.py. OpenWhisper_CPP collects only files it can authenticate and
// never by age (docs/port/differences.md, a16-cleanup).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

#include "base/atomic_file.h"
#include "prepare_support.h"
#include "update/testing/fake_mutex_api.h"
#include "update/update_contract.h"
#include "update_native/manifest.h"
#include "update_native/startup_recovery.h"
#include "update_native/terminal_cleanup.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace u = fv::update_native;
using fv::update::testing::FakeMutexApi;
using u::test::PrepareFixture;
using u::test::read_text;

// No process has this id: it is above Linux's limit, and Windows hands out small multiples of 4.
constexpr std::uint32_t kDeadPid = 0x7FFFFFF0;

std::uint32_t own_pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

bool start(PrepareFixture& f, FakeMutexApi& mutex, bool consent = true) {
    return u::recover_before_start(f.context, mutex, 1, 2, {},
                                   [consent](const u::UpdateJournal&) { return consent; });
}
} // namespace

// from
// tests/test_app_update_apply.py::TestAbandonedTransactions::test_a_finished_transaction_is_collected
TEST_CASE("a finished transaction is collected by the next start", "[update_native]") {
    PrepareFixture f;
    FakeMutexApi mutex;
    const auto id = f.prepare().transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    auto journal = u::load_journal(f.context, id);
    std::string active = "old app";
    SECTION("rolled back") {
        journal.state = u::TransactionState::RolledBack;
    }
    SECTION("superseded") {
        journal.state = u::TransactionState::Superseded;
    }
    SECTION("healthy") {
        fs::rename(f.install, paths.rollback_dir);
        fs::rename(paths.candidate_dir, f.install);
        journal.state = u::TransactionState::Healthy;
        journal.cleanup_rollback = true;
        active = "new app";
    }
    u::save_journal(f.context, journal);

    // The caller decides (it clears the recovery entry first); without its word nothing goes.
    CHECK_FALSE(start(f, mutex, false));
    CHECK(fs::is_directory(paths.transaction_dir));
    CHECK_FALSE(u::recover_before_start(f.context, mutex, 1, 2, {}));
    CHECK(fs::is_directory(paths.transaction_dir));

    CHECK_FALSE(start(f, mutex));
    CHECK_FALSE(fs::exists(paths.transaction_dir));
    CHECK_FALSE(fs::exists(paths.candidate_dir));
    CHECK_FALSE(fs::exists(paths.rollback_dir));
    CHECK(read_text(f.install / "openwhisper-cpp.exe") == active);
    CHECK(fs::is_regular_file(f.install / "unins000.exe"));
    // Every gate it took for the cleanup is given back before the application goes on.
    CHECK(mutex.owned().empty());
}

// from
// tests/test_app_update_apply.py::TestAbandonedTransactions::test_a_transaction_recovery_may_still_need_is_kept
TEST_CASE("a transaction recovery may still need is kept", "[update_native]") {
    PrepareFixture f;
    FakeMutexApi mutex;
    f.context.parent_exe = f.install / "openwhisper-cpp.exe";
    // Prepared by a process that is still there. Its image is not this installation's, so its
    // identity cannot be matched, and an unidentified process is never taken for a dead one.
    const auto handoff =
        u::prepare_update(f.context, f.archive, f.digest, "0.1.0", "0.1.1", own_pid(), 1);
    const auto id = handoff.transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    auto journal = u::load_journal(f.context, id);
    auto state = u::TransactionState::Prepared;
    bool dispatched = false;
    SECTION("prepared") {}
    SECTION("old moved") {
        state = u::TransactionState::OldMoved;
        dispatched = true;
    }
    SECTION("new active") {
        fs::rename(f.install, paths.rollback_dir);
        fs::rename(paths.candidate_dir, f.install);
        state = u::TransactionState::NewActive;
        dispatched = true;
    }
    journal.state = state;
    u::save_journal(f.context, journal);
    // Neither cleanup accepts it.
    CHECK_FALSE(u::cleanup_finished_transaction(f.context, journal));
    CHECK_THROWS(u::cleanup_terminal_candidate(f.context, journal));
    const auto launch = [](const fs::path&, const std::vector<std::string>&) {
        return true;
    };
    CHECK(u::recover_before_start(f.context, mutex, 1, 2, launch,
                                  [](const u::UpdateJournal&) { return true; }) == dispatched);
    CHECK(u::load_journal(f.context, id).state == state);
    CHECK(fs::is_regular_file(paths.transaction_dir / "openwhisper-cpp-updater.exe"));
    CHECK(fs::is_directory(dispatched && state == u::TransactionState::NewActive
                               ? paths.rollback_dir
                               : paths.candidate_dir));
}

// from
// tests/test_app_update_apply.py::TestAbandonedTransactions::test_an_unreadable_journal_is_left_for_recovery
// and, for OpenWhisper_CPP's rule,
// TestAbandonedTransactions::test_a_transaction_without_a_journal_is_collected
TEST_CASE("a transaction folder that cannot be read or has no journal is left alone",
          "[update_native]") {
    PrepareFixture f;
    FakeMutexApi mutex;
    const auto root = f.updates / "tx";
    const auto odd = root / std::string(32, 'f');
    const auto bare = root / std::string(32, 'c');
    fs::create_directories(odd);
    fs::create_directories(bare);
    fv::write_file_atomic(odd / "journal.json", "{not json");
    fv::write_file_atomic(bare / "openwhisper-cpp-updater.exe", "helper");
    // Age proves nothing about whose files these are: an old one stays too.
    const auto long_ago = fs::file_time_type::clock::now() - std::chrono::hours(24 * 365);
    fs::last_write_time(bare / "openwhisper-cpp-updater.exe", long_ago);
    fs::last_write_time(bare, long_ago);
    CHECK_FALSE(start(f, mutex));
    CHECK(read_text(odd / "journal.json") == "{not json");
    CHECK(read_text(bare / "openwhisper-cpp-updater.exe") == "helper");
    CHECK(mutex.owned().empty());
}

// from
// tests/test_app_update_apply.py::TestAbandonedTransactions::test_a_missing_transactions_directory_is_not_an_error
TEST_CASE("a start without any transaction has nothing to recover", "[update_native]") {
    PrepareFixture f;
    FakeMutexApi mutex;
    CHECK_FALSE(start(f, mutex));
    CHECK_FALSE(fs::exists(f.updates / "tx"));
    CHECK(mutex.created_calls.empty());
}

// from tests/test_update_resilience.py::test_locked_cleanup_retains_journal_and_retries
TEST_CASE("a cleanup that cannot finish keeps its journal and is retried", "[update_native]") {
    PrepareFixture f;
    FakeMutexApi mutex;
    const auto id = f.prepare().transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    auto journal = u::load_journal(f.context, id);
    fs::rename(f.install, paths.rollback_dir);
    fs::rename(paths.candidate_dir, f.install);
    journal.state = u::TransactionState::Healthy;
    journal.cleanup_rollback = true;
    u::save_journal(f.context, journal);
    // Something still holds the old tree: here a file a scanner left in it.
    fv::write_file_atomic(paths.rollback_dir / "scan.tmp", "held");
    CHECK_FALSE(start(f, mutex));
    CHECK(fs::is_directory(paths.rollback_dir));
    CHECK(u::load_journal(f.context, id).state == u::TransactionState::Healthy);
    fs::remove(paths.rollback_dir / "scan.tmp");
    CHECK_FALSE(start(f, mutex));
    CHECK_FALSE(fs::exists(paths.rollback_dir));
    CHECK_FALSE(fs::exists(paths.transaction_dir));
    CHECK(read_text(f.install / "openwhisper-cpp.exe") == "new app");
}

// OpenWhisper_CPP's counterpart of
// tests/test_app_update_apply.py::TestLockedFiles::test_cleanup_waits_out_the_updater_that_asked_for_it:
// there is no cleanup copy of the updater; the next start collects the transaction once the
// updater has let go of its gates.
TEST_CASE("a finished transaction waits for the updater that still holds the update gate",
          "[update_native]") {
    PrepareFixture f;
    FakeMutexApi mutex;
    const auto id = f.prepare().transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    auto journal = u::load_journal(f.context, id);
    journal.state = u::TransactionState::RolledBack;
    u::save_journal(f.context, journal);
    const auto gate = fv::update::release_identity().update_mutexes().front();
    mutex.held_elsewhere.insert(gate);
    bool asked = false;
    CHECK_FALSE(u::recover_before_start(f.context, mutex, 1, 2, {}, [&](const u::UpdateJournal&) {
        asked = true;
        return true;
    }));
    CHECK_FALSE(asked);
    CHECK(fs::is_regular_file(paths.transaction_dir / "openwhisper-cpp-updater.exe"));
    CHECK(fs::is_directory(paths.candidate_dir));
    mutex.held_elsewhere.erase(gate);
    CHECK_FALSE(start(f, mutex));
    CHECK_FALSE(fs::exists(paths.transaction_dir));
    CHECK_FALSE(fs::exists(paths.candidate_dir));
}

// from tests/test_update_resilience.py::test_preparing_journal_is_collected_after_parent_dies
TEST_CASE("a preparation whose application died is abandoned and collected", "[update_native]") {
    PrepareFixture f;
    FakeMutexApi mutex;
    f.context.parent_exe = f.install / "openwhisper-cpp.exe";
    const auto id =
        u::prepare_update(f.context, f.archive, f.digest, "0.1.0", "0.1.1", kDeadPid, 12345)
            .transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    SECTION("prepared") {}
    SECTION("still preparing, before its completion marker was written") {
        auto journal = u::load_journal(f.context, id);
        journal.state = u::TransactionState::Preparing;
        u::save_journal(f.context, journal);
        fs::remove(paths.candidate_dir / fv::update::sentinel_name());
    }
    CHECK_FALSE(start(f, mutex));
    CHECK(u::load_journal(f.context, id).state == u::TransactionState::Superseded);
    CHECK_FALSE(fs::exists(paths.candidate_dir));
    CHECK(read_text(f.install / "openwhisper-cpp.exe") == "old app");
    // The retired transaction itself goes with the start after.
    CHECK_FALSE(start(f, mutex));
    CHECK_FALSE(fs::exists(paths.transaction_dir));
    CHECK(mutex.owned().empty());
}
