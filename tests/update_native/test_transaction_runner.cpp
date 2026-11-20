// One run of the updater helper around a transaction (transaction_runner.h): what is reported,
// cleaned and started again once the outcome is decided. From OpenWhisper's
// tests/test_updater_helper.py, tests/test_update_resilience.py and the launch-directory tests of
// tests/test_app_update_apply.py, over the fake platform of transaction_support.h.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "base/atomic_file.h"
#include "prepare_support.h"
#include "testkit/testkit.h"
#include "transaction_support.h"
#include "update/update_contract.h"
#include "update_native/transaction_runner.h"

namespace fs = std::filesystem;
namespace u = fv::update_native;
using u::test::FakePlatform;
using u::test::Fixture;
using u::test::kId;

namespace {
// The actions of a run, recorded in the order they happen.
struct Recorder {
    std::vector<std::string> events;
    std::vector<std::string> reports;
    bool cleanup_fails = false;
    bool relaunch_fails = false;
    bool report_fails = false;
    int restores_at_relaunch = -1;
    u::RunnerActions actions(const FakePlatform& platform) {
        u::RunnerActions result;
        result.cleanup = [this](const u::UpdateJournal& journal) {
            events.push_back("cleanup " + std::string(u::to_string(journal.state)));
            if (cleanup_fails)
                throw std::system_error(std::make_error_code(std::errc::permission_denied),
                                        "old files are still locked");
        };
        result.relaunch = [this, &platform](const u::UpdateJournal& journal) {
            events.push_back("relaunch " + std::string(u::to_string(journal.state)));
            restores_at_relaunch = platform.restores;
            if (relaunch_fails)
                throw u::UpdateError("The recovered application could not be started.");
        };
        result.report = [this](std::string_view message) {
            events.emplace_back("report");
            if (report_fails)
                throw std::system_error(std::make_error_code(std::errc::io_error),
                                        "Could not write the update error");
            reports.emplace_back(message);
        };
        return result;
    }
};

// Restores the working directory a test changed.
struct WorkingDirectory {
    fs::path saved = fs::current_path();
    ~WorkingDirectory() {
        std::error_code ignored;
        fs::current_path(saved, ignored);
    }
};
} // namespace

// from tests/test_updater_helper.py::test_helper_commits_prepared_journal
TEST_CASE("the helper commits a prepared journal and then collects its leftovers",
          "[update_native]") {
    Fixture f;
    Recorder r;
    CHECK(u::run_transaction(f.context, f.journal, f.platform, false, r.actions(f.platform)) ==
          u::TransactionState::Healthy);
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::New);
    // The new version is the health-checked process itself: nothing is started again, and a
    // success leaves no message for the next launch.
    CHECK(r.events == std::vector<std::string>{"cleanup healthy"});
    CHECK(r.reports.empty());
}

// from tests/test_updater_helper.py::test_helper_recover_path
TEST_CASE("the helper recovers an interrupted journal, says so and starts the old version",
          "[update_native]") {
    Fixture f;
    f.journal.state = u::TransactionState::OldMoved;
    u::save_journal(f.context, f.journal);
    fs::rename(f.paths.install_dir, f.paths.rollback_dir);
    fs::rename(f.paths.candidate_dir, f.paths.install_dir);
    Recorder r;
    CHECK(u::run_transaction(f.context, f.journal, f.platform, true, r.actions(f.platform)) ==
          u::TransactionState::RolledBack);
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
    CHECK(r.events ==
          std::vector<std::string>{"report", "cleanup rolled_back", "relaunch rolled_back"});
    CHECK(r.reports ==
          std::vector<std::string>{"The previous update was interrupted and was rolled back."});
    // The data went back before the old version was started.
    CHECK(r.restores_at_relaunch == 1);
}

TEST_CASE("a recovery that confirms the new version starts it without a message",
          "[update_native]") {
    Fixture f;
    f.journal.state = u::TransactionState::NewActive;
    u::save_journal(f.context, f.journal);
    fs::rename(f.paths.install_dir, f.paths.rollback_dir);
    fs::rename(f.paths.candidate_dir, f.paths.install_dir);
    f.platform.health_recorded = true;
    Recorder r;
    CHECK(u::run_transaction(f.context, f.journal, f.platform, true, r.actions(f.platform)) ==
          u::TransactionState::Healthy);
    CHECK(r.events == std::vector<std::string>{"cleanup healthy", "relaunch healthy"});
    CHECK(r.reports.empty());
    CHECK(f.platform.restores == 0);
}

// from tests/test_update_resilience.py::test_completed_rollback_never_rewinds_later_user_work
TEST_CASE("recovering a finished rollback neither rewinds data nor reports it again",
          "[update_native]") {
    Fixture f;
    f.journal.state = u::TransactionState::RolledBack;
    u::save_journal(f.context, f.journal);
    Recorder r;
    CHECK(u::run_transaction(f.context, f.journal, f.platform, true, r.actions(f.platform)) ==
          u::TransactionState::RolledBack);
    CHECK(f.platform.restores == 0);
    CHECK(r.reports.empty());
}

// from tests/test_update_resilience.py::test_failed_commit_restores_settings_before_old_app_returns
// and
// tests/test_app_update_apply.py::TestCommitAndRecover::test_injected_failure_after_old_moved_rolls_back
TEST_CASE("a failed commit is reported and the old version returns to restored data",
          "[update_native]") {
    Fixture f;
    f.platform.health_result = false;
    Recorder r;
    std::string message;
    try {
        u::run_transaction(f.context, f.journal, f.platform, false, r.actions(f.platform));
    } catch (const u::ReportedByApplication& error) {
        message = error.what();
    }
    CHECK(message == "The new version started but did not become ready.");
    CHECK(f.journal.state == u::TransactionState::RolledBack);
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
    CHECK(r.events ==
          std::vector<std::string>{"report", "cleanup rolled_back", "relaunch rolled_back"});
    CHECK(r.reports == std::vector<std::string>{message});
    CHECK(r.restores_at_relaunch == 1);
}

TEST_CASE("a rollback whose old version cannot be started stays the helper's to report",
          "[update_native]") {
    Fixture f;
    f.platform.health_result = false;
    Recorder r;
    r.relaunch_fails = true;
    bool by_application = false;
    std::string message;
    try {
        u::run_transaction(f.context, f.journal, f.platform, false, r.actions(f.platform));
    } catch (const u::ReportedByApplication&) {
        by_application = true;
    } catch (const u::UpdateError& error) {
        message = error.what();
    }
    CHECK_FALSE(by_application);
    CHECK(message == "The new version started but did not become ready.");
}

// The helper stays silent only when the relaunched application has the failure to show
// (apply_error.txt). Every other failure of a commit is the helper's to report, so the user
// always hears of it once.
TEST_CASE("a failure the application was not told about stays the helper's to report",
          "[update_native]") {
    Fixture f;
    f.platform.health_result = false;
    Recorder r;
    std::string expected = "The new version started but did not become ready.";
    SECTION("the update error could not be left for the next launch") {
        r.report_fails = true;
    }
    SECTION("the rollback itself failed") {
        f.platform.fail_restore = true;
        expected = "The update failed and rollback could not restore the previous "
                   "installation: The rollback tree is locked.";
    }
    bool by_application = false;
    std::string message;
    try {
        u::run_transaction(f.context, f.journal, f.platform, false, r.actions(f.platform));
    } catch (const u::ReportedByApplication&) {
        by_application = true;
    } catch (const u::UpdateError& error) {
        message = error.what();
    }
    CHECK_FALSE(by_application);
    CHECK(message == expected);
}

// from
// tests/test_app_update_apply.py::TestLiveParent::test_running_parent_is_not_reported_as_a_failed_rollback
TEST_CASE("a commit refused because the application is still open moves and starts nothing",
          "[update_native]") {
    Fixture f;
    f.context.parent_exe = f.paths.install_dir / "fiorivoce.exe";
    f.journal.parent_exe = f.context.parent_exe;
    f.journal.parent_pid = 17;
    f.journal.parent_creation_time = 123;
    u::save_journal(f.context, f.journal);
    f.platform.parent_gone = false;
    Recorder r;
    bool by_application = false;
    std::string message;
    try {
        u::run_transaction(f.context, f.journal, f.platform, false, r.actions(f.platform));
    } catch (const u::ReportedByApplication&) {
        by_application = true;
    } catch (const u::UpdateError& error) {
        message = error.what();
    }
    // The application is still there to be closed by its user: no second copy is started behind
    // it, and the helper itself has to say why nothing happened.
    CHECK_FALSE(by_application);
    CHECK(message == u::parent_still_running_message());
    CHECK(r.events == std::vector<std::string>{"report"});
    CHECK(r.reports == std::vector<std::string>{message});
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
    CHECK(u::load_journal(f.context, kId).state == u::TransactionState::Prepared);
}

// from tests/test_updater_helper.py::test_cleanup_failure_is_not_reported_as_an_update_failure
TEST_CASE("a cleanup failure is not reported as an update failure", "[update_native]") {
    Fixture f;
    Recorder r;
    r.cleanup_fails = true;
    CHECK(u::run_transaction(f.context, f.journal, f.platform, false, r.actions(f.platform)) ==
          u::TransactionState::Healthy);
    CHECK(r.reports.empty());
    // The journal stays, so a later start collects what could not be removed now.
    CHECK(u::load_journal(f.context, kId).state == u::TransactionState::Healthy);
}

// from
// tests/test_update_resilience.py::test_valid_old_install_is_relaunched_even_when_cleanup_is_locked
TEST_CASE("a valid old installation is started again even when cleanup is locked",
          "[update_native]") {
    Fixture f;
    f.journal.state = u::TransactionState::OldMoved;
    u::save_journal(f.context, f.journal);
    // The crash came before the first rename finished: the old tree is in place, and a stray
    // duplicate of it sits where the rollback would be.
    Fixture::make_tree(f.paths.rollback_dir, "old");
    Recorder r;
    r.cleanup_fails = true;
    CHECK(u::run_transaction(f.context, f.journal, f.platform, true, r.actions(f.platform)) ==
          u::TransactionState::RolledBack);
    CHECK(FakePlatform::tree(f.paths.install_dir) == u::TreeState::Old);
    CHECK(fs::exists(f.paths.transaction_dir));
    CHECK(u::load_journal(f.context, kId).state == u::TransactionState::RolledBack);
    CHECK(r.events.back() == "relaunch rolled_back");
    CHECK(std::ranges::count(r.events, std::string("relaunch rolled_back")) == 1);
}

TEST_CASE("the application is started again only from an authenticated tree", "[update_native]") {
    u::test::PrepareFixture f;
    const auto journal = u::load_journal(f.context, f.prepare().transaction_id);
    std::vector<fs::path> started;
    const auto start = [&](const fs::path& executable) {
        started.push_back(executable);
        return true;
    };
    u::relaunch_verified(f.context, journal, start);
    CHECK(started == std::vector<fs::path>{f.install / "fiorivoce.exe"});
    CHECK_THROWS_AS(u::relaunch_verified(f.context, journal, [](const fs::path&) { return false; }),
                    u::UpdateError);
    // An executable that is neither release's is never started.
    fv::write_file_atomic(f.install / "fiorivoce.exe", "something else");
    CHECK_THROWS_AS(u::relaunch_verified(f.context, journal, start), u::UpdateError);
    CHECK(started.size() == 1);
}

// from tests/test_app_update_apply.py::TestLaunchDirectory::test_a_source_run_changes_directory
TEST_CASE("a helper started in the install folder leaves it for the updates root",
          "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const WorkingDirectory restore;
    const auto install = temp.path() / "programs" / "fiorivoce";
    const auto updates = temp.path() / "appdata" / "updates";
    fs::create_directories(install);
    fs::current_path(install);
    CHECK(u::leave_launch_directory(updates));
    CHECK(fs::equivalent(fs::current_path(), updates));
    // Nothing more to leave.
    CHECK_FALSE(u::leave_launch_directory(updates));
    CHECK(fs::equivalent(fs::current_path(), updates));
}

// from
// tests/test_app_update_apply.py::TestLaunchDirectory::test_a_helper_already_under_the_updates_root_is_left_alone
TEST_CASE("a helper already under the updates root is left alone", "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const WorkingDirectory restore;
    const auto updates = temp.path() / "appdata" / "updates";
    const auto transaction =
        updates / fv::update::kTransactionsDirname / "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    fs::create_directories(transaction);
    fs::current_path(transaction);
    CHECK_FALSE(u::leave_launch_directory(updates));
    CHECK(fs::equivalent(fs::current_path(), transaction));
    // A sibling whose name merely starts like the root is not inside it.
    const auto lookalike = temp.path() / "appdata" / "updates-old";
    fs::create_directories(lookalike);
    fs::current_path(lookalike);
    CHECK(u::leave_launch_directory(updates));
    CHECK(fs::equivalent(fs::current_path(), updates));
}
