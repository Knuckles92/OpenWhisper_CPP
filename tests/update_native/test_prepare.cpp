#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "base/atomic_file.h"
#include "base/cancel.h"
#include "base/utf8.h"
#include "download/verified_files.h"
#include "prepare_support.h"
#include "testkit/testkit.h"
#include "update/testing/fake_mutex_api.h"
#include "update/update_contract.h"
#include "update_native/archive.h"
#include "update_native/manifest.h"
#include "update_native/prepare.h"
#include "update_native/startup_recovery.h"
#include "update_native/terminal_cleanup.h"

namespace {
namespace fs = std::filesystem;
namespace u = fv::update_native;
using u::test::PrepareFixture;
using u::test::read_text;

// A candidate, rollback or failed tree, or an unpublished extraction, beside the installation.
bool update_tree_left(const fs::path& parent) {
    for (const auto& entry : fs::directory_iterator(parent)) {
        const auto name = fv::path_to_utf8(entry.path().filename());
        if (name.find(".new-") != std::string::npos || name.find(".old-") != std::string::npos ||
            name.find(".failed-") != std::string::npos ||
            name.starts_with(".openwhisper-cpp-candidate-"))
            return true;
    }
    return false;
}
bool transaction_left(const PrepareFixture& f) {
    const auto root = f.updates / "tx";
    return fs::exists(root) && !fs::is_empty(root);
}

TEST_CASE("native preparation preserves Inno metadata and stages the installed helper",
          "[update_native]") {
    PrepareFixture f;
    const auto handoff = f.prepare();
    REQUIRE(handoff.args.size() == 3);
    CHECK(handoff.args[0] == "--apply");
    CHECK(handoff.args[1] == fv::update::kTransactionArg);
    const auto paths = u::journal_paths(f.context, handoff.args[2]);
    const auto journal = u::load_journal(f.context, handoff.args[2]);
    REQUIRE(journal.state == u::TransactionState::Prepared);
    CHECK(handoff.setup_exe == paths.transaction_dir / "openwhisper-cpp-updater.exe");
    for (const auto* name : {"openwhisper-cpp-updater.exe", "libcrypto-3-x64.dll"})
        CHECK(fv::download::sha256_file(paths.transaction_dir / name) ==
              fv::download::sha256_file(f.install / name));
    for (const auto* name : {"unins000.exe", "unins000.dat"}) {
        REQUIRE(journal.compatibility_files.contains(name));
        CHECK(fv::download::sha256_file(paths.candidate_dir / name) ==
              journal.compatibility_files[name]["sha256"].get<std::string>());
    }
    CHECK(fs::is_regular_file(paths.candidate_dir / fv::update::sentinel_name()));
    CHECK(fv::download::sha256_file(f.install / "openwhisper-cpp.exe") == journal.old_exe_sha256);
    CHECK(fv::download::sha256_file(paths.candidate_dir / "openwhisper-cpp.exe") ==
          journal.new_exe_sha256);
    CHECK_FALSE(fs::exists(paths.rollback_dir));
}

TEST_CASE("normal installed startup recovers an interrupted update using the original helper",
          "[update_native]") {
    PrepareFixture f;
    fv::update::testing::FakeMutexApi mutex;
    const auto handoff = f.prepare();
    const auto id = handoff.transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    auto journal = u::load_journal(f.context, id);
    journal.state = u::TransactionState::OldMoved;
    u::save_journal(f.context, journal);
    bool launchable = true;
    auto expected = u::TransactionState::OldMoved;
    SECTION("rename intent still has the original active directory") {}
    SECTION("old tree moved to rollback") {
        fs::rename(f.install, paths.rollback_dir);
    }
    // from
    // tests/test_update_resilience.py::test_startup_dispatches_recovery_before_acquiring_app_lock
    SECTION("the new tree is already active") {
        fs::rename(f.install, paths.rollback_dir);
        fs::rename(paths.candidate_dir, f.install);
        journal.state = expected = u::TransactionState::NewActive;
        u::save_journal(f.context, journal);
    }
    SECTION("helper replacement fails before launch") {
        fv::write_file_atomic(handoff.setup_exe, "replaced helper");
        launchable = false;
    }
    SECTION("an unlisted library fails before launch") {
        fv::write_file_atomic(paths.transaction_dir / "unlisted.dll", "unexpected");
        launchable = false;
    }
    bool called = false;
    const auto launch = [&](const fs::path& executable, const std::vector<std::string>& args) {
        called = true;
        CHECK(executable == handoff.setup_exe);
        const std::vector<std::string> command{
            "--recover", "--transaction-id", id, "--wait-for-launcher", "11", "22"};
        CHECK(args == command);
        return true;
    };
    if (launchable)
        CHECK(u::recover_before_start(f.context, mutex, 11, 22, launch));
    else
        CHECK_THROWS(u::recover_before_start(f.context, mutex, 11, 22, launch));
    CHECK(called == launchable);
    CHECK(u::load_journal(f.context, id).state == expected);
    // The helper is dispatched before this launch takes any gate: it needs them all itself.
    CHECK(mutex.created_calls.empty());
    CHECK(mutex.owned().empty());
}

TEST_CASE("abandoned preparation leaves the installed application alone at startup",
          "[update_native]") {
    PrepareFixture f;
    fv::update::testing::FakeMutexApi mutex;
    CHECK_FALSE(u::recover_before_start(f.context, mutex, 1, 2, {}));
    const auto id = f.prepare().transaction_id;
    CHECK_FALSE(u::recover_before_start(f.context, mutex, 1, 2, {}));
    CHECK(u::load_journal(f.context, id).state == u::TransactionState::Superseded);
    CHECK(fs::is_regular_file(f.install / "openwhisper-cpp.exe"));
}

TEST_CASE("a healthy update retires only the authenticated previous installation",
          "[update_native]") {
    PrepareFixture f;
    const auto id = f.prepare().transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    auto journal = u::load_journal(f.context, id);
    journal.state = u::TransactionState::Healthy;
    journal.cleanup_rollback = true;
    fs::rename(f.install, paths.rollback_dir);
    fs::rename(paths.candidate_dir, f.install);
    SECTION("previous Inno installation without a marker") {
        CHECK(u::cleanup_healthy_rollback(f.context, journal));
        CHECK_FALSE(fs::exists(paths.rollback_dir));
    }
    SECTION("unknown previous installation files remain") {
        fv::write_file_atomic(paths.rollback_dir / "my-document.txt", "keep");
        CHECK_THROWS(u::cleanup_healthy_rollback(f.context, journal));
        CHECK(fs::exists(paths.rollback_dir / "my-document.txt"));
    }
    CHECK(fs::exists(f.install / "openwhisper-cpp.exe"));
}

TEST_CASE("discarded transaction releases only known staged helper and snapshot files",
          "[update_native]") {
    PrepareFixture f;
    const auto id = f.prepare().transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    auto journal = u::load_journal(f.context, id);
    journal.state = u::TransactionState::Superseded;
    u::save_journal(f.context, journal);
    REQUIRE(u::cleanup_terminal_candidate(f.context, journal));
    SECTION("unknown file is preserved") {
        fv::write_file_atomic(paths.transaction_dir / "my-recording.wav", "personal");
        CHECK_FALSE(u::cleanup_finished_transaction(f.context, journal));
        CHECK(fs::exists(paths.transaction_dir / "my-recording.wav"));
    }
    SECTION("finished transaction is collected") {
        CHECK(u::cleanup_finished_transaction(f.context, journal));
        CHECK_FALSE(fs::exists(paths.transaction_dir));
    }
    CHECK(fs::exists(f.install / "openwhisper-cpp.exe"));
}

TEST_CASE("native preparation refuses changed inputs before any handoff", "[update_native]") {
    PrepareFixture f;
    SECTION("release digest") {
        f.digest.assign(64, '0');
    }
    SECTION("installed runtime") {
        fv::write_file_atomic(f.install / "libcrypto-3-x64.dll", "changed");
    }
    SECTION("mismatched registration") {
        f.context.expected_registration->install_location = f.data;
    }
    SECTION("unregistered uninstaller") {
        f.context.expected_registration->uninstall_string = "cmd.exe";
    }
    REQUIRE_THROWS(f.prepare());
    CHECK(fs::is_regular_file(f.install / "openwhisper-cpp.exe"));
}
// from tests/test_app_update_apply.py::TestPreserveAndEligibility::test_hklm_is_not_native_eligible
TEST_CASE("an all-users registration is sent to the setup before anything is staged",
          "[update_native]") {
    PrepareFixture f;
    f.context.expected_registration->hive = fv::update::RegistryHive::LocalMachine;
    CHECK_THROWS_AS(f.prepare(), u::RequiresSetup);
    CHECK_FALSE(transaction_left(f));
    CHECK_FALSE(update_tree_left(f.folder.path()));
    CHECK(read_text(f.install / "openwhisper-cpp.exe") == "old app");
}

// from
// tests/test_app_update_apply.py::TestPrepareCandidate::test_stale_registered_version_is_repaired_by_native_update
TEST_CASE("a stale registered version does not stop a native update", "[update_native]") {
    PrepareFixture f;
    // An earlier native update can leave only DisplayVersion behind. It describes the install; it
    // is not its identity. The commit writes the new version and a rollback restores this one.
    f.context.expected_registration->display_version = "0.0.9";
    const auto id = f.prepare().transaction_id;
    const auto journal = u::load_journal(f.context, id);
    CHECK(journal.state == u::TransactionState::Prepared);
    CHECK(journal.old_version == "0.1.0");
    CHECK(journal.old_display_version == "0.0.9");
    CHECK(journal.new_version == "0.1.1");
}

// from
// tests/test_app_update_apply.py::TestPrepareCancel::test_cancel_after_extraction_still_cancels
// The archive goes with the rest (app_update_apply.py _discard_preparation,
// _remove_transaction_payloads), so each attempt here downloads the release again first.
TEST_CASE("a cancel after the extraction still cancels and leaves nothing behind",
          "[update_native]") {
    PrepareFixture f;
    // "extracted": after the extraction; "preserved": after the uninstaller files were hashed and
    // copied; "staged": after the helper was hashed and staged.
    for (const auto* point : {"extracted", "preserved", "staged"}) {
        CAPTURE(point);
        f.download_again();
        fv::CancelSource cancel;
        bool reached = false;
        CHECK_THROWS_AS(u::prepare_update(f.context, f.archive, f.digest, "0.1.0", "0.1.1", 0, 0,
                                          cancel.token(),
                                          [&](std::string_view at) {
                                              if (at == point) {
                                                  reached = true;
                                                  cancel.cancel();
                                              }
                                          }),
                        u::Canceled);
        CHECK(reached);
        CHECK_FALSE(update_tree_left(f.folder.path()));
        CHECK_FALSE(transaction_left(f));
        CHECK_FALSE(fs::exists(f.archive));
        CHECK(read_text(f.install / "openwhisper-cpp.exe") == "old app");
        CHECK(fs::is_regular_file(f.install / "unins000.exe"));
    }
    // A later attempt downloads again and prepares as if nothing had happened.
    f.download_again();
    CHECK(u::load_journal(f.context, f.prepare().transaction_id).state ==
          u::TransactionState::Prepared);
}

TEST_CASE("a preparation that fails part way removes what it wrote", "[update_native]") {
    PrepareFixture f;
    CHECK_THROWS_WITH(u::prepare_update(f.context, f.archive, f.digest, "0.1.0", "0.1.1", 0, 0, {},
                                        [](std::string_view at) {
                                            if (at == "preserved")
                                                throw u::UpdateError("The disk went away.");
                                        }),
                      "The disk went away.");
    CHECK_FALSE(update_tree_left(f.folder.path()));
    CHECK_FALSE(transaction_left(f));
    CHECK_FALSE(fs::exists(f.archive));
    CHECK(read_text(f.install / "openwhisper-cpp.exe") == "old app");
    f.download_again();
    CHECK(u::load_journal(f.context, f.prepare().transaction_id).state ==
          u::TransactionState::Prepared);
}

// app_update_apply.py _discard_preparation ends the journal rolled_back; OpenWhisper_CPP's name for
// a transaction that never reached the commit is superseded. A transaction that cannot be collected
// (here: a file nobody knows in it) keeps its journal, which shows that end state, and the archive
// is removed all the same.
TEST_CASE("a discarded preparation ends superseded and loses its archive even when its "
          "transaction stays",
          "[update_native]") {
    PrepareFixture f;
    std::string id;
    const auto plant = [&](std::string_view at) {
        if (at != "staged")
            return;
        const auto root = f.updates / "tx";
        for (const auto& entry : fs::directory_iterator(root)) {
            id = fv::path_to_utf8(entry.path().filename());
            fv::write_file_atomic(entry.path() / "my-recording.wav", "personal");
        }
        throw u::UpdateError("The disk went away.");
    };
    CHECK_THROWS_WITH(
        u::prepare_update(f.context, f.archive, f.digest, "0.1.0", "0.1.1", 0, 0, {}, plant),
        "The disk went away.");
    REQUIRE(id.size() == 32);
    const auto paths = u::journal_paths(f.context, id);
    CHECK(u::load_journal(f.context, id).state == u::TransactionState::Superseded);
    CHECK(fs::exists(paths.transaction_dir / "my-recording.wav"));
    CHECK_FALSE(fs::exists(paths.candidate_dir));
    CHECK_FALSE(fs::exists(f.archive));
    CHECK(read_text(f.install / "openwhisper-cpp.exe") == "old app");
}

// A failure before the journal exists is OpenWhisper's first except clause: only the transaction
// folder goes, and the archive stays.
TEST_CASE("a preparation refused before its journal keeps the archive", "[update_native]") {
    PrepareFixture f;
    f.context.expected_registration->hive = fv::update::RegistryHive::LocalMachine;
    CHECK_THROWS_AS(f.prepare(), u::RequiresSetup);
    CHECK(fs::is_regular_file(f.archive));
}

TEST_CASE("an abandoned hand-off releases its candidate and transaction", "[update_native]") {
    PrepareFixture f;
    const auto handoff = f.prepare();
    const auto paths = u::journal_paths(f.context, handoff.transaction_id);
    REQUIRE(fs::is_directory(paths.candidate_dir));
    // A hand-off that names another program is not this installation's.
    auto foreign = handoff;
    foreign.setup_exe = f.folder.path() / "openwhisper-cpp-updater.exe";
    CHECK_THROWS_AS(u::abandon_prepared_update(f.context, foreign), u::UpdateError);
    CHECK(fs::is_directory(paths.candidate_dir));
    u::abandon_prepared_update(f.context, handoff);
    CHECK_FALSE(fs::exists(paths.candidate_dir));
    CHECK_FALSE(fs::exists(paths.transaction_dir));
    CHECK(read_text(f.install / "openwhisper-cpp.exe") == "old app");
    // app_update_apply.py abandon_prepared_update: the archive goes too, and the next attempt
    // downloads again.
    CHECK_FALSE(fs::exists(f.archive));
    f.download_again();
    CHECK(u::load_journal(f.context, f.prepare().transaction_id).state ==
          u::TransactionState::Prepared);
}

TEST_CASE("an abandoned hand-off never touches a transaction that has started", "[update_native]") {
    PrepareFixture f;
    const auto handoff = f.prepare();
    const auto paths = u::journal_paths(f.context, handoff.transaction_id);
    auto journal = u::load_journal(f.context, handoff.transaction_id);
    journal.state = u::TransactionState::OldMoved;
    u::save_journal(f.context, journal);
    u::abandon_prepared_update(f.context, handoff);
    CHECK(u::load_journal(f.context, handoff.transaction_id).state ==
          u::TransactionState::OldMoved);
    CHECK(fs::is_directory(paths.candidate_dir));
}
} // namespace
