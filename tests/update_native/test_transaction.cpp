#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "base/atomic_file.h"
#include "base/cancel.h"
#include "testkit/testkit.h"
#include "transaction_support.h"
#include "update_native/manifest.h"
#include "update_native/transaction.h"

namespace fs = std::filesystem;
namespace u = fv::update_native;
using u::test::FakePlatform;
using u::test::Fixture;
using u::test::kId;

TEST_CASE("native transaction persists intent before each rename and confirms health",
          "[update_native]") {
    Fixture f;
    CHECK(u::apply_prepared(f.context, f.journal, f.platform) == u::TransactionState::Healthy);
    CHECK(f.platform.states == std::vector<u::TransactionState>{u::TransactionState::OldMoved,
                                                                u::TransactionState::NewActive,
                                                                u::TransactionState::Healthy});
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::New);
    CHECK(FakePlatform::tree(f.paths.rollback_dir) == u::TreeState::Old);
    CHECK_FALSE(f.platform.recovery);
    CHECK(f.platform.restores == 0);
    CHECK(f.platform.registrations == 1);
    CHECK(u::load_journal(f.context, kId).state == u::TransactionState::Healthy);
}

TEST_CASE("faults before health restore a verified old installation", "[update_native]") {
    for (const auto point : {"after_old_moved_intent", "after_old_moved", "after_new_active_intent",
                             "after_new_active", "before_health"}) {
        CAPTURE(point);
        Fixture f;
        CHECK_THROWS_AS(u::apply_prepared(f.context, f.journal, f.platform, {},
                                          [point](std::string_view at) {
                                              if (at == point)
                                                  throw u::UpdateError("Injected failure.");
                                          }),
                        u::UpdateError);
        CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
        CHECK(f.journal.state == u::TransactionState::RolledBack);
        CHECK_FALSE(f.platform.recovery);
        CHECK(u::recover_transaction(f.context, f.journal, f.platform) ==
              u::TransactionState::RolledBack);
    }
}

TEST_CASE("a failure after confirmed health never rewinds the new installation",
          "[update_native]") {
    Fixture f;
    CHECK_THROWS_AS(u::apply_prepared(f.context, f.journal, f.platform, {},
                                      [](std::string_view at) {
                                          if (at == "after_health")
                                              throw u::UpdateError("Injected failure.");
                                      }),
                    u::UpdateError);
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::New);
    CHECK(f.platform.restores == 0);
    CHECK(f.platform.recovery);
    CHECK(u::recover_transaction(f.context, f.journal, f.platform) == u::TransactionState::Healthy);
    CHECK(f.platform.restores == 0);
}

TEST_CASE("failed health and cancellation after moving the old tree roll back", "[update_native]") {
    SECTION("failed health") {
        Fixture f;
        f.platform.health_result = false;
        CHECK_THROWS_AS(u::apply_prepared(f.context, f.journal, f.platform), u::UpdateError);
        CHECK(f.platform.stopped == 1);
        CHECK(f.platform.restores == 1);
        CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
    }
    SECTION("cancel after first rename") {
        Fixture f;
        fv::CancelSource cancel;
        CHECK_THROWS_AS(u::apply_prepared(f.context, f.journal, f.platform, cancel.token(),
                                          [&cancel](std::string_view at) {
                                              if (at == "after_old_moved")
                                                  cancel.cancel();
                                          }),
                        u::UpdateError);
        CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
        CHECK(f.journal.state == u::TransactionState::RolledBack);
    }
}

TEST_CASE("native recovery never overwrites a foreign or existing rollback tree",
          "[update_native]") {
    SECTION("existing rollback prevents apply") {
        Fixture f;
        Fixture::make_tree(f.paths.rollback_dir, "old");
        CHECK_THROWS_AS(u::apply_prepared(f.context, f.journal, f.platform), u::UpdateError);
        CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
        CHECK(FakePlatform::tree(f.paths.rollback_dir) == u::TreeState::Old);
    }
    SECTION("later repair owns the active tree") {
        Fixture f;
        Fixture::make_tree(f.paths.rollback_dir, "old");
        fv::write_file_atomic(f.paths.install_dir / "version.txt", "foreign");
        f.journal.state = u::TransactionState::NewActive;
        CHECK(u::recover_transaction(f.context, f.journal, f.platform) ==
              u::TransactionState::Superseded);
        CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Foreign);
        CHECK(FakePlatform::tree(f.paths.rollback_dir) == u::TreeState::Old);
        CHECK(f.platform.restores == 0);
    }
}

TEST_CASE("invalid journal or a live parent cannot start a native swap", "[update_native]") {
    SECTION("the journal names an unrelated installation") {
        Fixture f;
        f.journal.app_dir = f.temp.path() / "unrelated";
        CHECK_THROWS_AS(u::apply_prepared(f.context, f.journal, f.platform), u::UpdateError);
        CHECK(f.platform.states.empty());
        CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
        CHECK(FakePlatform::tree(f.paths.candidate_dir) == u::TreeState::New);
    }
    SECTION("the independently matched parent is still running") {
        Fixture f;
        f.context.parent_exe = f.paths.install_dir / "fiorivoce.exe";
        f.journal.parent_exe = f.context.parent_exe;
        f.journal.parent_pid = 17;
        f.journal.parent_creation_time = 123;
        f.platform.parent_gone = false;
        std::string message;
        try {
            u::apply_prepared(f.context, f.journal, f.platform);
        } catch (const u::UpdateError& error) {
            message = error.what();
        }
        // The parent is never ended to make room (differences.md, a16-parent-exit): the update
        // stops, nothing has moved, and the failure is not worded as a rollback.
        CHECK(message == u::parent_still_running_message());
        CHECK(message.find("did not close") != std::string::npos);
        CHECK(message.find("ollback") == std::string::npos);
        CHECK(f.platform.states.empty());
        CHECK(f.platform.snapshots == 0);
        CHECK_FALSE(f.platform.recovery);
        CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
        CHECK(FakePlatform::tree(f.paths.candidate_dir) == u::TreeState::New);
        CHECK(u::load_journal(f.context, kId).state == u::TransactionState::Prepared);
    }
}

TEST_CASE("failed rollback preserves intent for a later recovery", "[update_native]") {
    Fixture f;
    f.platform.health_result = false;
    f.platform.fail_restore = true;
    CHECK_THROWS_AS(u::apply_prepared(f.context, f.journal, f.platform), u::UpdateError);
    CHECK(f.platform.recovery);
    CHECK(FakePlatform::tree(f.paths.rollback_dir) == u::TreeState::Old);
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Missing);
    CHECK(u::load_journal(f.context, kId).state == u::TransactionState::NewActive);
    f.platform.fail_restore = false;
    CHECK(u::recover_transaction(f.context, f.journal, f.platform) ==
          u::TransactionState::RolledBack);
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
    CHECK_FALSE(f.platform.recovery);
}

TEST_CASE("an altered activated tree is never launched", "[update_native]") {
    Fixture f;
    CHECK_THROWS_AS(u::apply_prepared(f.context, f.journal, f.platform, {},
                                      [&f](std::string_view at) {
                                          if (at == "after_new_active")
                                              fv::write_file_atomic(
                                                  f.paths.install_dir / "version.txt", "foreign");
                                      }),
                    u::UpdateError);
    CHECK_FALSE(f.platform.health_recorded);
    CHECK(f.journal.state == u::TransactionState::Superseded);
    CHECK(FakePlatform::tree(f.paths.rollback_dir) == u::TreeState::Old);
}

TEST_CASE("recovery finishes restoring data after the old tree was moved back", "[update_native]") {
    Fixture f;
    f.journal.state = u::TransactionState::NewActive;
    CHECK(u::recover_transaction(f.context, f.journal, f.platform) ==
          u::TransactionState::RolledBack);
    CHECK(f.platform.restores == 1);
    CHECK(f.platform.registry_restores == 1);
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
}

// from
// tests/test_app_update_apply.py::TestCommitAndRecover::test_tampered_candidate_is_rejected_before_swap
TEST_CASE("a candidate that no longer verifies is refused before anything moves",
          "[update_native]") {
    Fixture f;
    fv::write_file_atomic(f.paths.candidate_dir / "version.txt", "tampered");
    CHECK_THROWS_AS(u::apply_prepared(f.context, f.journal, f.platform), u::UpdateError);
    CHECK(f.platform.states.empty());
    CHECK(f.platform.snapshots == 0);
    CHECK_FALSE(f.platform.recovery);
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
    CHECK(FakePlatform::tree(f.paths.rollback_dir) == u::TreeState::Missing);
    CHECK(u::load_journal(f.context, kId).state == u::TransactionState::Prepared);
}

// from
// tests/test_app_update_apply.py::TestCommitAndRecover::test_recover_prepared_abandons_candidate
// and tests/test_update_resilience.py::test_retired_transaction_cannot_run_after_setup
TEST_CASE("recovering a prepared transaction retires it without touching the installation",
          "[update_native]") {
    Fixture f;
    f.platform.recovery = true;
    CHECK(u::recover_transaction(f.context, f.journal, f.platform) ==
          u::TransactionState::Superseded);
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
    CHECK(FakePlatform::tree(f.paths.rollback_dir) == u::TreeState::Missing);
    CHECK(f.platform.restores == 0);
    CHECK(f.platform.registry_restores == 0);
    CHECK_FALSE(f.platform.recovery);
    CHECK(u::load_journal(f.context, kId).state == u::TransactionState::Superseded);
    // A retired transaction can neither be applied nor recovered into a swap any more.
    CHECK_THROWS_AS(u::apply_prepared(f.context, f.journal, f.platform), u::UpdateError);
    f.platform.states.clear();
    CHECK(u::recover_transaction(f.context, f.journal, f.platform) ==
          u::TransactionState::Superseded);
    CHECK(f.platform.states.empty());
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
    CHECK(FakePlatform::tree(f.paths.candidate_dir) == u::TreeState::New);
}

// from
// tests/test_app_update_apply.py::TestCommitAndRecover::test_recover_prepared_after_old_rename_restores_rollback
TEST_CASE("recovery returns an old tree that was moved before the journal said so",
          "[update_native]") {
    Fixture f;
    fs::rename(f.paths.install_dir, f.paths.rollback_dir);
    CHECK(u::recover_transaction(f.context, f.journal, f.platform) ==
          u::TransactionState::RolledBack);
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
    CHECK(FakePlatform::tree(f.paths.rollback_dir) == u::TreeState::Missing);
    CHECK(u::load_journal(f.context, kId).state == u::TransactionState::RolledBack);
}

// from
// tests/test_app_update_apply.py::TestCommitAndRecover::test_recover_old_moved_after_candidate_rename_rolls_back
TEST_CASE("recovery after both renames puts the old tree back", "[update_native]") {
    Fixture f;
    f.journal.state = u::TransactionState::OldMoved;
    u::save_journal(f.context, f.journal);
    fs::rename(f.paths.install_dir, f.paths.rollback_dir);
    fs::rename(f.paths.candidate_dir, f.paths.install_dir);
    f.platform.recovery = true;
    CHECK(u::recover_transaction(f.context, f.journal, f.platform) ==
          u::TransactionState::RolledBack);
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
    // The unconfirmed new tree was kept aside, never overwritten.
    CHECK(FakePlatform::tree(f.paths.install_dir.parent_path() / "failed-test") ==
          u::TreeState::New);
    CHECK(f.platform.restores == 1);
    CHECK(f.platform.registry_restores == 1);
    CHECK_FALSE(f.platform.recovery);
}

// from
// tests/test_app_update_apply.py::TestCommitAndRecover::test_unconfirmed_new_tree_without_rollback_keeps_recovery
TEST_CASE("an unconfirmed new tree without a rollback keeps its recovery entry",
          "[update_native]") {
    Fixture f;
    f.journal.state = u::TransactionState::NewActive;
    u::save_journal(f.context, f.journal);
    fs::remove_all(f.paths.install_dir);
    fs::rename(f.paths.candidate_dir, f.paths.install_dir);
    f.platform.recovery = true;
    std::string message;
    try {
        u::recover_transaction(f.context, f.journal, f.platform);
    } catch (const u::UpdateError& error) {
        message = error.what();
    }
    CHECK(message == "The interrupted update has no validated rollback tree.");
    // Nothing is given up: the entry that runs recovery again and the journal both stay.
    CHECK(f.platform.recovery);
    CHECK(f.platform.states.empty());
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::New);
    CHECK(u::load_journal(f.context, kId).state == u::TransactionState::NewActive);
}

// from tests/test_update_resilience.py::test_failed_commit_restores_settings_before_old_app_returns
TEST_CASE("a commit that fails after activation restores the data it snapshotted",
          "[update_native]") {
    Fixture f;
    CHECK_THROWS_WITH(u::apply_prepared(f.context, f.journal, f.platform, {},
                                        [](std::string_view at) {
                                            if (at == "after_new_active")
                                                throw u::UpdateError("failed migration");
                                        }),
                      "failed migration");
    CHECK(f.platform.snapshots == 1);
    CHECK(f.platform.restores == 1);
    CHECK(f.platform.registry_restores == 1);
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
    CHECK(f.journal.state == u::TransactionState::RolledBack);
}

// from tests/test_update_resilience.py::test_completed_rollback_never_rewinds_later_user_work
TEST_CASE("a finished rollback is never replayed over later work", "[update_native]") {
    Fixture f;
    f.journal.state = u::TransactionState::RolledBack;
    u::save_journal(f.context, f.journal);
    CHECK(u::recover_transaction(f.context, f.journal, f.platform) ==
          u::TransactionState::RolledBack);
    CHECK(f.platform.restores == 0);
    CHECK(f.platform.registry_restores == 0);
    CHECK(f.platform.states.empty());
}
