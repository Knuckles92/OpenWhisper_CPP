#include "base/atomic_file.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "download/verified_files.h"
#include "prepare_support.h"
#include "testkit/testkit.h"
#include "update/testing/fake_mutex_api.h"
#include "update/update_contract.h"
#include "update_native/archive.h"
#include "update_native/native_platform.h"
#include "update_native/prepare.h"
#include "update_native/storage_actions.h"
#include "update_native/terminal_cleanup.h"
#include "update_native/transaction_runner.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <system_error>
#ifdef _WIN32
#include <cstdio>
#include <share.h>
#endif

namespace fs = std::filesystem;
namespace u = fv::update_native;
namespace {
struct FakeRegistry final : u::RegistryActions {
    bool recovery = true;
    int restores = 0;
    int updates = 0;
    void verify(const u::UpdateJournal&) override {}
    void install_recovery(const u::UpdateJournal&) override { recovery = true; }
    void clear_recovery(const u::UpdateJournal&) override { recovery = false; }
    void update(const u::UpdateJournal&) override { ++updates; }
    void restore(const u::UpdateJournal&) override { ++restores; }
};
using u::test::PrepareFixture;
using u::test::read_text;
} // namespace
TEST_CASE("native platform recovers an interrupted temporary installation", "[update_native]") {
    fv::testkit::TempDataRoot temp;
    constexpr auto id = "0123456789abcdef0123456789abcdef";
    u::JournalContext context{temp.path() / "data", temp.path() / "programs" / "OpenWhisper_CPP",
                              "{01234567-89AB-CDEF-0123-456789ABCDEF}"};
    fv::update::InstallRegistration reg;
    reg.key_path = "Software\\OpenWhisper_CPP\\Uninstall";
    reg.install_location = context.install_dir;
    context.expected_registration = reg;
    context.expected_runonce_key = "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
    const auto paths = u::journal_paths(context, id);
    fs::create_directories(paths.transaction_dir);
    fs::create_directories(paths.install_dir);
    fs::create_directories(paths.candidate_dir);
    fv::write_file_atomic(paths.install_dir / "openwhisper-cpp.exe", "old");
    fv::write_file_atomic(paths.candidate_dir / "openwhisper-cpp.exe", "new");
    fv::write_file_atomic(paths.candidate_dir / "openwhisper-cpp-updater.exe", "helper");
    u::write_manifest(paths.candidate_dir, "0.1.1");
    fv::write_file_atomic(paths.candidate_dir / fv::update::sentinel_name(), "0.1.1");
    auto journal = u::make_journal(context, id);
    journal.old_version = journal.old_display_version = "0.1.0";
    journal.new_version = "0.1.1";
    journal.old_exe_sha256 =
        fv::download::sha256_file(paths.install_dir / "openwhisper-cpp.exe", true);
    journal.new_exe_sha256 =
        fv::download::sha256_file(paths.candidate_dir / "openwhisper-cpp.exe", true);
    {
        u::StorageActions actions(context, id);
        actions.storage().acquire_data_lease();
        actions.storage().snapshot_with_pinned_sources();
        actions.move_old_to_rollback();
        actions.move_candidate_to_install();
        journal.state = u::TransactionState::NewActive;
        u::save_journal(context, journal);
    }
    FakeRegistry registry;
    fv::update::testing::FakeMutexApi mutex;
    u::NativePlatform platform(context, id, registry, mutex, fv::update::release_identity());
    SECTION("unacknowledged activation") {}
    // from
    // tests/test_app_update_apply.py::TestCommitAndRecover::test_empty_health_file_is_not_success
    SECTION("an empty acknowledgement file is not a confirmation") {
        fv::write_file_atomic(paths.transaction_dir / fv::update::kHealthName, "");
    }
    SECTION("another transaction's token is not a confirmation") {
        fv::write_file_atomic(paths.transaction_dir / fv::update::kHealthName,
                              std::string(32, '0'));
    }
    SECTION("acknowledged activation with a damaged library or helper") {
        fv::write_file_atomic(paths.transaction_dir / fv::update::kHealthName,
                              journal.health_token);
        fv::write_file_atomic(paths.install_dir / "openwhisper-cpp-updater.exe", "damaged");
    }
    CHECK(u::recover_transaction(context, journal, platform) == u::TransactionState::RolledBack);
    CHECK(registry.restores == 1);
    CHECK_FALSE(registry.recovery);
    CHECK(fv::download::sha256_file(paths.install_dir / "openwhisper-cpp.exe", true) ==
          journal.old_exe_sha256);
}

TEST_CASE("prepared Inno files survive activation and rollback", "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const auto install = temp.path() / "installed app";
    const auto data = temp.path() / "data";
    const auto release_dir = temp.path() / "release";
    const auto archive = data / "updates" / "release.zip";
    for (const auto& dir : {install, release_dir, archive.parent_path()})
        fs::create_directories(dir);
    for (const auto& dir : {install, release_dir}) {
        const std::string prefix = dir == install ? "old" : "new";
        fv::write_file_atomic(dir / "openwhisper-cpp.exe", prefix + " app");
        fv::write_file_atomic(dir / "openwhisper-cpp-updater.exe", prefix + " helper");
        fv::write_file_atomic(dir / "libcrypto-3-x64.dll", prefix + " library");
    }
    u::write_manifest(install, "0.1.0");
    fv::write_file_atomic(install / "unins000.exe", "Inno executable");
    fv::write_file_atomic(install / "unins000.dat", "Inno state");
    u::pack_archive(release_dir, "0.1.1", archive);
    const auto& identity = fv::update::release_identity();
    fv::update::InstallRegistration reg;
    reg.key_path = identity.uninstall_key_path();
    reg.install_location = install;
    reg.uninstall_string = "\"" + fv::path_to_utf8(install / "unins000.exe") + "\"";
    reg.display_version = "0.1.0";
    u::JournalContext context{
        data, install, identity.app_id,
        {},   reg,     "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce"};
    const auto handoff = u::prepare_update(
        context, archive, fv::download::sha256_file(archive, true), "0.1.0", "0.1.1", 0, 0);
    const auto id = handoff.args.at(2);
    auto journal = u::load_journal(context, id);
    const auto candidate = u::journal_paths(context, id).candidate_dir;
    {
        u::PinnedTree pinned(candidate);
        CHECK_NOTHROW(pinned.verify_release(journal.new_version, journal.new_exe_sha256,
                                            journal.compatibility_files));
    }
    fv::write_file_atomic(candidate / "unins000.dat", "tampered");
    {
        u::PinnedTree pinned(candidate);
        CHECK_THROWS(pinned.verify_release(journal.new_version, journal.new_exe_sha256,
                                           journal.compatibility_files));
    }
    fv::write_file_atomic(candidate / "unins000.dat", "Inno state");
    fv::write_file_atomic(candidate / fv::update::sentinel_name(), "wrong-version");
    {
        u::PinnedTree pinned(candidate);
        CHECK_THROWS(pinned.verify_release(journal.new_version, journal.new_exe_sha256,
                                           journal.compatibility_files));
    }
    fv::write_file_atomic(candidate / fv::update::sentinel_name(), journal.new_version);
    FakeRegistry registry;
    fv::update::testing::FakeMutexApi mutex;
    u::NativePlatform platform(context, id, registry, mutex, identity);
    bool activated = false;
    CHECK_THROWS(
        u::apply_prepared(context, journal, platform, {}, [&activated](std::string_view point) {
            if (point == "before_health") {
                activated = true;
                throw std::runtime_error("Simulated interruption before health.");
            }
        }));
    CHECK(activated);
    CHECK(journal.state == u::TransactionState::RolledBack);
    CHECK(fs::exists(install / "unins000.exe"));
    CHECK(fs::exists(install / "unins000.dat"));
}

TEST_CASE("terminal cleanup preserves unknown files and removes only a verified candidate",
          "[update_native]") {
    fv::testkit::TempDataRoot temp;
    constexpr auto id = "0123456789abcdef0123456789abcdef";
    u::JournalContext context{temp.path() / "data", temp.path() / "programs" / "OpenWhisper_CPP",
                              "{01234567-89AB-CDEF-0123-456789ABCDEF}"};
    fv::update::InstallRegistration reg;
    reg.key_path = "Software\\OpenWhisper_CPP\\Uninstall";
    reg.install_location = context.install_dir;
    context.expected_registration = reg;
    context.expected_runonce_key = "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
    const auto paths = u::journal_paths(context, id);
    fs::create_directories(paths.transaction_dir);
    fs::create_directories(paths.install_dir);
    fs::create_directories(paths.candidate_dir / "platforms");
    fv::write_file_atomic(paths.install_dir / "keep.txt", "user data");
    fv::write_file_atomic(paths.candidate_dir / "openwhisper-cpp.exe", "new");
    fv::write_file_atomic(paths.candidate_dir / "openwhisper-cpp-updater.exe", "helper");
    fv::write_file_atomic(paths.candidate_dir / "platforms" / "qwindows.dll", "plugin");
    u::write_manifest(paths.candidate_dir, "0.1.1");
    fv::write_file_atomic(paths.candidate_dir / fv::update::sentinel_name(), "0.1.1");
    auto journal = u::make_journal(context, id);
    journal.old_version = journal.old_display_version = "0.1.0";
    journal.new_version = "0.1.1";
    journal.new_exe_sha256 =
        fv::download::sha256_file(paths.candidate_dir / "openwhisper-cpp.exe", true);
    journal.state = u::TransactionState::Superseded;
    fv::write_file_atomic(paths.candidate_dir / "user.txt", "preserve");
    CHECK_THROWS(u::cleanup_terminal_candidate(context, journal));
    CHECK(fs::exists(paths.candidate_dir / "user.txt"));
    fs::remove(paths.candidate_dir / "user.txt");
    fs::create_directory(paths.candidate_dir / "unmanaged");
    CHECK_THROWS(u::cleanup_terminal_candidate(context, journal));
    CHECK(fs::exists(paths.candidate_dir / "unmanaged"));
    fs::remove(paths.candidate_dir / "unmanaged");
    CHECK(u::cleanup_terminal_candidate(context, journal));
    CHECK_FALSE(fs::exists(paths.candidate_dir));
    CHECK(fs::exists(paths.install_dir / "keep.txt"));
}

// from tests/test_update_resilience.py::test_failed_commit_restores_settings_before_old_app_returns
TEST_CASE("a commit that fails after activation puts the user's settings back", "[update_native]") {
    PrepareFixture f;
    const auto settings = f.data / fv::identity().settings_file;
    fv::write_file_atomic(settings, R"({"original": true})");
    const auto id = f.prepare().transaction_id;
    auto journal = u::load_journal(f.context, id);
    FakeRegistry registry;
    fv::update::testing::FakeMutexApi mutex;
    u::NativePlatform platform(f.context, id, registry, mutex, fv::update::release_identity());
    CHECK_THROWS(u::apply_prepared(f.context, journal, platform, {}, [&](std::string_view point) {
        if (point == "after_new_active") {
            // What a new version's failed migration would leave behind.
            fv::write_file_atomic(settings, R"({"incompatible": true})");
            throw u::UpdateError("failed migration");
        }
    }));
    CHECK(journal.state == u::TransactionState::RolledBack);
    CHECK(read_text(settings) == R"({"original": true})");
    CHECK(read_text(f.install / "openwhisper-cpp.exe") == "old app");
    CHECK(registry.restores == 1);
    CHECK_FALSE(registry.recovery);
    CHECK(mutex.owned().empty());
}

// from
// tests/test_app_update_apply.py::TestCommitAndRecover::test_tampered_candidate_is_rejected_before_swap
TEST_CASE("a candidate changed after its preparation is rejected before the swap",
          "[update_native]") {
    PrepareFixture f;
    const auto id = f.prepare().transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    auto journal = u::load_journal(f.context, id);
    SECTION("its executable") {
        fv::write_file_atomic(paths.candidate_dir / "openwhisper-cpp.exe", "tampered");
    }
    SECTION("one of its libraries") {
        fv::write_file_atomic(paths.candidate_dir / "libcrypto-3-x64.dll", "tampered");
    }
    SECTION("a file that was never part of the release") {
        fv::write_file_atomic(paths.candidate_dir / "extra.dll", "planted");
    }
    FakeRegistry registry;
    registry.recovery = false;
    fv::update::testing::FakeMutexApi mutex;
    u::NativePlatform platform(f.context, id, registry, mutex, fv::update::release_identity());
    CHECK_THROWS_AS(u::apply_prepared(f.context, journal, platform), u::UpdateError);
    CHECK(read_text(f.install / "openwhisper-cpp.exe") == "old app");
    CHECK_FALSE(fs::exists(paths.rollback_dir));
    CHECK(u::load_journal(f.context, id).state == u::TransactionState::Prepared);
    // Nothing was snapshotted and no recovery entry was written: nothing had started.
    CHECK_FALSE(fs::exists(paths.transaction_dir / "data"));
    CHECK_FALSE(registry.recovery);
    CHECK(mutex.owned().empty());
}

// from tests/test_update_resilience.py::test_recovery_never_overwrites_independent_setup_repair
TEST_CASE("recovery never overwrites an installation a later setup repaired", "[update_native]") {
    PrepareFixture f;
    const auto id = f.prepare().transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    auto journal = u::load_journal(f.context, id);
    {
        u::StorageActions actions(f.context, id);
        actions.storage().acquire_data_lease();
        actions.storage().snapshot_with_pinned_sources();
        actions.move_old_to_rollback();
        actions.move_candidate_to_install();
        journal.state = u::TransactionState::NewActive;
        journal.cleanup_rollback = true;
        u::save_journal(f.context, journal);
    }
    // Neither the old nor the new release's program: somebody else installed this one.
    fv::write_file_atomic(f.install / "openwhisper-cpp.exe", "newer setup repair");
    FakeRegistry registry;
    fv::update::testing::FakeMutexApi mutex;
    u::NativePlatform platform(f.context, id, registry, mutex, fv::update::release_identity());
    CHECK(u::recover_transaction(f.context, journal, platform) == u::TransactionState::Superseded);
    CHECK(read_text(f.install / "openwhisper-cpp.exe") == "newer setup repair");
    CHECK(registry.restores == 0);
    CHECK_FALSE(registry.recovery);
    // The old tree is not put back over it, and it is still there to inspect.
    CHECK(fs::is_directory(paths.rollback_dir));
}

// from
// tests/test_app_update_apply.py::TestLockedFiles::test_hashing_the_app_it_just_closed_waits_for_the_handle
TEST_CASE("authenticating the installation waits out a lock on the application that just closed",
          "[update_native]") {
    PrepareFixture f;
    const auto id = f.prepare().transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    const auto journal = u::load_journal(f.context, id);
    FakeRegistry registry;
    fv::update::testing::FakeMutexApi mutex;
    u::NativePlatform platform(f.context, id, registry, mutex, fv::update::release_identity());
    u::LockRetry retry;
    retry.timeout = std::chrono::seconds(30);
#ifdef _WIN32
    // The image is still open without sharing, as it is for a moment after its process ends.
    std::FILE* held = _wfsopen((f.install / "openwhisper-cpp.exe").c_str(), L"rb", _SH_DENYRW);
    REQUIRE(held != nullptr);
    const auto release = [&] {
        std::fclose(held);
    };
#else
    // No sharing modes here: the tree cannot be opened while a link stands in its place, and
    // the test says that this failure is the lock.
    const auto parked = f.folder.path() / "parked";
    fs::rename(f.install, parked);
    fs::create_directory_symlink(parked, f.install);
    retry.options.locked = [](const std::system_error&) {
        return true;
    };
    const auto release = [&] {
        fs::remove(f.install);
        fs::rename(parked, f.install);
    };
#endif
    int waits = 0;
    retry.options.wait = [&](std::chrono::milliseconds) {
        if (++waits == 2)
            release();
    };
    platform.set_lock_retry(retry);
    CHECK(platform.active_tree(paths, journal) == u::TreeState::Old);
    CHECK(waits == 2);
}

#ifndef _WIN32
// The health check starts the new version itself. A script can stand in for it here; on Windows
// the real launch is the owner's installed demonstration.
// from tests/test_app_update_apply.py::TestCommitAndRecover::test_swap_and_cleanup
TEST_CASE("a prepared update is swapped in, confirmed by the new version and cleaned up",
          "[update_native]") {
    // $2 is the health token and $4 the transaction: acknowledge as the application does, by
    // publishing the token in the transaction folder, and stay alive like a started application.
    PrepareFixture f("#!/bin/sh\n"
                     "tx=\"$(dirname \"$0\")/../data/updates/tx/$4\"\n"
                     "printf %s \"$2\" > \"$tx/healthy.part\" && "
                     "mv \"$tx/healthy.part\" \"$tx/healthy\"\n"
                     "exec sleep 2\n");
    // An earlier update left a stale DisplayVersion behind: the commit writes the new one.
    f.context.expected_registration->display_version = "0.0.9";
    const auto id = f.prepare().transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    fs::permissions(paths.candidate_dir / "openwhisper-cpp.exe", fs::perms::owner_exec,
                    fs::perm_options::add);
    auto journal = u::load_journal(f.context, id);
    const auto new_app = read_text(paths.candidate_dir / "openwhisper-cpp.exe");
    FakeRegistry registry;
    fv::update::testing::FakeMutexApi mutex;
    u::NativePlatform platform(f.context, id, registry, mutex, fv::update::release_identity());
    u::RunnerActions actions;
    int relaunches = 0;
    actions.cleanup = [&](const u::UpdateJournal& finished) {
        CHECK(u::cleanup_healthy_rollback(f.context, finished));
    };
    actions.relaunch = [&](const u::UpdateJournal&) {
        ++relaunches;
    };
    CHECK(u::run_transaction(f.context, journal, platform, false, actions) ==
          u::TransactionState::Healthy);
    CHECK(read_text(f.install / "openwhisper-cpp.exe") == new_app);
    CHECK(read_text(f.install / "unins000.exe") == "Inno image");
    CHECK_FALSE(fs::exists(paths.candidate_dir));
    CHECK_FALSE(fs::exists(paths.rollback_dir));
    CHECK(u::load_journal(f.context, id).state == u::TransactionState::Healthy);
    CHECK(registry.updates == 1);
    CHECK(registry.restores == 0);
    CHECK_FALSE(registry.recovery);
    // The health-checked process is the running application: nothing else is started.
    CHECK(relaunches == 0);
    CHECK(mutex.owned().empty());
    // The transaction's own folder goes once the helper is out of it.
    CHECK(u::cleanup_finished_transaction(f.context, journal));
    CHECK_FALSE(fs::exists(paths.transaction_dir));
}

// from tests/test_update_resilience.py::test_health_process_exit_fails_without_waiting_for_timeout
TEST_CASE("a new version that exits fails its health check and the old one returns",
          "[update_native]") {
    PrepareFixture f("#!/bin/sh\nexit 3\n");
    const auto id = f.prepare().transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    fs::permissions(paths.candidate_dir / "openwhisper-cpp.exe", fs::perms::owner_exec,
                    fs::perm_options::add);
    auto journal = u::load_journal(f.context, id);
    FakeRegistry registry;
    fv::update::testing::FakeMutexApi mutex;
    u::NativePlatform platform(f.context, id, registry, mutex, fv::update::release_identity());
    const auto started = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(u::apply_prepared(f.context, journal, platform), u::UpdateError);
    // The health wait is two minutes; a process that is gone ends it at once.
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(30));
    CHECK(journal.state == u::TransactionState::RolledBack);
    CHECK(read_text(f.install / "openwhisper-cpp.exe") == "old app");
    // The new tree that failed was kept aside, then collected as this release's own files.
    CHECK(fs::is_directory(paths.failed_dir));
    CHECK(u::cleanup_terminal_candidate(f.context, journal));
    CHECK_FALSE(fs::exists(paths.failed_dir));
    CHECK(registry.restores == 1);
}
#endif
