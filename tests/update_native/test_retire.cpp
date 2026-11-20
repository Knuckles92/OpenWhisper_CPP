// What the installer's helper does to this installation's native transactions before its files
// are replaced (retire.h), from OpenWhisper's tests/test_update_resilience.py.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "base/atomic_file.h"
#include "base/identity.h"
#include "prepare_support.h"
#include "update/testing/fake_mutex_api.h"
#include "update_native/manifest.h"
#include "update_native/native_platform.h"
#include "update_native/retire.h"
#include "update_native/storage_actions.h"

namespace {
namespace fs = std::filesystem;
namespace u = fv::update_native;
using fv::update::testing::FakeMutexApi;
using u::test::PrepareFixture;
using u::test::read_text;

struct QuietRegistry final : u::RegistryActions {
    int restores = 0;
    bool recovery = true;
    void verify(const u::UpdateJournal&) override {}
    void install_recovery(const u::UpdateJournal&) override { recovery = true; }
    void clear_recovery(const u::UpdateJournal&) override { recovery = false; }
    void update(const u::UpdateJournal&) override {}
    void restore(const u::UpdateJournal&) override { ++restores; }
};
} // namespace

// from tests/test_update_resilience.py::test_retired_transaction_cannot_run_after_setup
TEST_CASE("a transaction retired for setup can no longer run", "[update_native]") {
    PrepareFixture f;
    FakeMutexApi mutex;
    const auto& release = fv::update::release_identity();
    const auto id = f.prepare().transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    // Only the installer, holding its gate, may retire anything.
    CHECK_THROWS_AS(u::retire_transactions(f.context, mutex, release, {}), u::UpdateError);
    CHECK(u::load_journal(f.context, id).state == u::TransactionState::Prepared);
    mutex.held_elsewhere.insert(release.setup_mutex());
    u::retire_transactions(f.context, mutex, release, {});
    auto journal = u::load_journal(f.context, id);
    CHECK(journal.state == u::TransactionState::Superseded);
    CHECK_FALSE(fs::exists(paths.candidate_dir));
    // The helper staged for it finds nothing it may do, whichever way it is started.
    QuietRegistry registry;
    u::NativePlatform platform(f.context, id, registry, mutex, release,
                               u::SetupGate::HeldByInstaller);
    CHECK(u::recover_transaction(f.context, journal, platform) == u::TransactionState::Superseded);
    CHECK_THROWS_AS(u::apply_prepared(f.context, journal, platform), u::UpdateError);
    CHECK(read_text(f.install / "fiorivoce.exe") == "old app");
    CHECK(registry.restores == 0);
    // Retiring again changes nothing.
    u::retire_transactions(f.context, mutex, release, {});
    CHECK(u::load_journal(f.context, id).state == u::TransactionState::Superseded);
}

TEST_CASE("setup puts a half-swapped update back before it replaces any file", "[update_native]") {
    PrepareFixture f;
    FakeMutexApi mutex;
    const auto& release = fv::update::release_identity();
    const auto settings = f.data / fv::identity().settings_file;
    fv::write_file_atomic(settings, "before the update");
    const auto id = f.prepare().transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    auto journal = u::load_journal(f.context, id);
    {
        // As the helper left it when it was interrupted: data saved, both trees moved.
        u::StorageActions actions(f.context, id);
        actions.storage().acquire_data_lease();
        actions.storage().snapshot_with_pinned_sources();
        actions.move_old_to_rollback();
        actions.move_candidate_to_install();
        journal.state = u::TransactionState::NewActive;
        journal.cleanup_rollback = true;
        u::save_journal(f.context, journal);
    }
    fv::write_file_atomic(settings, "written by the unconfirmed version");
    mutex.held_elsewhere.insert(release.setup_mutex());
    // An interrupted swap cannot be retired without the means to undo it.
    CHECK_THROWS_AS(u::retire_transactions(f.context, mutex, release, {}), u::UpdateError);
    QuietRegistry registry;
    u::retire_transactions(f.context, mutex, release, [&](u::UpdateJournal& interrupted) {
        u::NativePlatform platform(f.context, interrupted.transaction_id, registry, mutex, release,
                                   u::SetupGate::HeldByInstaller);
        (void)u::recover_transaction(f.context, interrupted, platform);
    });
    CHECK(u::load_journal(f.context, id).state == u::TransactionState::RolledBack);
    CHECK(read_text(f.install / "fiorivoce.exe") == "old app");
    CHECK(read_text(settings) == "before the update");
    CHECK(registry.restores == 1);
    CHECK_FALSE(registry.recovery);
    // Neither the rollback tree nor the new tree that was moved aside is left beside it.
    CHECK_FALSE(fs::exists(paths.rollback_dir));
    CHECK_FALSE(fs::exists(paths.failed_dir));
    CHECK_FALSE(fs::exists(paths.candidate_dir));
    // The installer still holds its gate; the helper took and returned everything else.
    CHECK(mutex.owned().empty());
}
