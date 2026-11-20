// The downloaded archive goes with every terminal cleanup, as OpenWhisper's
// _remove_transaction_payloads deletes it (services/app_update_apply.py): the helper's cleanup
// after a healthy or rolled-back run, the next start's collection of a finished or abandoned
// transaction, and setup's retirement. Pruning by age (update::prune_stale_downloads) is left for
// what a crash leaves behind (open item "update_native: after a healthy, rolled-back or
// recovered transaction").

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "base/atomic_file.h"
#include "prepare_support.h"
#include "transaction_support.h"
#include "update/testing/fake_mutex_api.h"
#include "update/update_contract.h"
#include "update_native/retire.h"
#include "update_native/startup_recovery.h"
#include "update_native/transaction_runner.h"

namespace {
namespace fs = std::filesystem;
namespace u = fv::update_native;
using fv::update::testing::FakeMutexApi;
using u::test::Fixture;
using u::test::PrepareFixture;

// A downloaded archive in the updates folder, named in the fixture's journal.
fs::path download_for(Fixture& f) {
    const auto archive = f.paths.updates_root / "OpenWhisper_CPP-0.1.1-win64.zip";
    fv::write_file_atomic(archive, "zip bytes");
    f.journal.archive_path = archive;
    u::save_journal(f.context, f.journal);
    return archive;
}

u::RunnerActions quiet_actions() {
    u::RunnerActions actions;
    actions.cleanup = [](const u::UpdateJournal&) {
    };
    actions.relaunch = [](const u::UpdateJournal&) {
    };
    actions.report = [](std::string_view) {
    };
    return actions;
}
} // namespace

TEST_CASE("sweep A37: the helper removes the archive once the update is healthy",
          "[update_native]") {
    Fixture f;
    const auto archive = download_for(f);
    REQUIRE(fs::is_regular_file(archive));

    CHECK(u::run_transaction(f.context, f.journal, f.platform, false, quiet_actions()) ==
          u::TransactionState::Healthy);
    CHECK_FALSE(fs::exists(archive));
}

TEST_CASE("sweep A37: the helper removes the archive after recovering by rollback",
          "[update_native]") {
    Fixture f;
    const auto archive = download_for(f);
    f.journal.state = u::TransactionState::OldMoved;
    u::save_journal(f.context, f.journal);
    fs::rename(f.paths.install_dir, f.paths.rollback_dir);
    fs::rename(f.paths.candidate_dir, f.paths.install_dir);

    CHECK(u::run_transaction(f.context, f.journal, f.platform, true, quiet_actions()) ==
          u::TransactionState::RolledBack);
    CHECK_FALSE(fs::exists(archive));
}

TEST_CASE("sweep A37: a start that collects a finished transaction removes its archive",
          "[update_native]") {
    PrepareFixture f;
    FakeMutexApi mutex;
    const auto id = f.prepare().transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    REQUIRE(fs::is_regular_file(f.archive));
    auto journal = u::load_journal(f.context, id);
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
    }
    u::save_journal(f.context, journal);

    CHECK_FALSE(u::recover_before_start(f.context, mutex, 1, 2, {},
                                        [](const u::UpdateJournal&) { return true; }));
    CHECK_FALSE(fs::exists(paths.transaction_dir));
    CHECK_FALSE(fs::exists(f.archive));
}

TEST_CASE("sweep A37: setup's retirement of a prepared update removes its archive",
          "[update_native]") {
    PrepareFixture f;
    FakeMutexApi mutex;
    const auto& release = fv::update::release_identity();
    (void)f.prepare();
    REQUIRE(fs::is_regular_file(f.archive));
    mutex.held_elsewhere.insert(release.setup_mutex());

    u::retire_transactions(f.context, mutex, release, {});

    CHECK_FALSE(fs::exists(f.archive));
}
