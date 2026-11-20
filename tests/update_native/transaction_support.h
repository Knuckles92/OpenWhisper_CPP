#pragma once

// A transaction platform over disposable directories and a journal fixture around it: the trees
// are told apart by a version.txt, everything else is counted. Shared by the transaction tests
// and the tests of the helper that runs a transaction.

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/atomic_file.h"
#include "base/cancel.h"
#include "testkit/testkit.h"
#include "update_native/manifest.h"
#include "update_native/transaction.h"

namespace fv::update_native::test {
namespace fs = std::filesystem;
namespace u = fv::update_native;

inline constexpr std::string_view kId = "0123456789abcdef0123456789abcdef";
inline constexpr std::string_view kAppId = "{01234567-89AB-CDEF-0123-456789ABCDEF}";
inline constexpr std::string_view kRunOnce =
    "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
struct Guard final : u::TransactionGates {};

// Only disposable directories under this fixture are renamed. A production platform is not
// provided here: its handle-relative no-reparse and process-gate rules must be implemented first.
struct FakePlatform final : u::TransactionPlatform {
    explicit FakePlatform(const u::JournalContext& context) : context(context) {}
    std::unique_ptr<u::TransactionGates> acquire_gates() override {
        return std::make_unique<Guard>();
    }
    bool parent_exited(const u::JournalPaths&, const u::UpdateJournal&,
                       const fv::CancelToken&) override {
        return parent_gone;
    }
    void verify_registration(const u::JournalPaths&, const u::UpdateJournal&) override {
        if (!registration_ok)
            throw u::UpdateError("The install registration is no longer valid.");
    }
    static u::TreeState tree(const fs::path& path) {
        if (!fs::exists(path))
            return u::TreeState::Missing;
        std::ifstream in(path / "version.txt");
        std::string text;
        in >> text;
        if (text == "old")
            return u::TreeState::Old;
        if (text == "new")
            return u::TreeState::New;
        return u::TreeState::Foreign;
    }
    u::TreeState active_tree(const u::JournalPaths& p, const u::UpdateJournal&) override {
        return tree(p.install_dir);
    }
    u::TreeState candidate_tree(const u::JournalPaths& p, const u::UpdateJournal&) override {
        return tree(p.candidate_dir);
    }
    u::TreeState rollback_tree(const u::JournalPaths& p, const u::UpdateJournal&) override {
        return tree(p.rollback_dir);
    }
    void snapshot_data(const u::JournalPaths&, const u::UpdateJournal&) override { ++snapshots; }
    void restore_data(const u::JournalPaths&, const u::UpdateJournal&) override { ++restores; }
    void install_recovery(const u::JournalPaths&, const u::UpdateJournal&) override {
        recovery = true;
    }
    void clear_recovery(const u::JournalPaths&, const u::UpdateJournal&) override {
        recovery = false;
    }
    void persist(const u::JournalPaths&, const u::UpdateJournal& journal) override {
        u::save_journal(context, journal);
        states.push_back(journal.state);
    }
    static void move(const fs::path& from, const fs::path& to) {
        if (fs::exists(to))
            throw u::UpdateError("The update destination already exists.");
        fs::rename(from, to);
    }
    void move_old_to_rollback(const u::JournalPaths& p) override {
        move(p.install_dir, p.rollback_dir);
    }
    void move_candidate_to_install(const u::JournalPaths& p) override {
        move(p.candidate_dir, p.install_dir);
    }
    void move_new_to_failed(const u::JournalPaths& p) override {
        move(p.install_dir, p.install_dir.parent_path() / "failed-test");
    }
    void move_rollback_to_install(const u::JournalPaths& p) override {
        if (fail_restore)
            throw u::UpdateError("The rollback tree is locked.");
        move(p.rollback_dir, p.install_dir);
    }
    bool launch_and_check_health(const u::JournalPaths&, const u::UpdateJournal&,
                                 const fv::CancelToken&) override {
        health_recorded = health_result;
        return health_result;
    }
    bool health_confirmed(const u::JournalPaths&, const u::UpdateJournal&) override {
        return health_recorded;
    }
    void update_registration(const u::JournalPaths&, const u::UpdateJournal&) override {
        ++registrations;
    }
    void restore_registration(const u::JournalPaths&, const u::UpdateJournal&) override {
        ++registry_restores;
    }
    void stop_failed_health_process(const u::JournalPaths&, const u::UpdateJournal&) override {
        ++stopped;
    }
    const u::JournalContext& context;
    bool parent_gone = true, registration_ok = true, health_result = true, health_recorded = false;
    bool fail_restore = false;
    bool recovery = false;
    int snapshots = 0, restores = 0, registrations = 0, registry_restores = 0, stopped = 0;
    std::vector<u::TransactionState> states;
};

struct Fixture {
    fv::testkit::TempDataRoot temp;
    u::JournalContext context;
    u::UpdateJournal journal;
    u::JournalPaths paths;
    FakePlatform platform;
    Fixture()
        : context{temp.path() / "data", temp.path() / "programs" / "OpenWhisper_CPP",
                  std::string(kAppId)},
          journal(u::make_journal(context, kId)), paths(u::journal_paths(context, kId)),
          platform(context) {
        journal.old_version = "0.1.0";
        journal.new_version = "0.1.1";
        journal.old_display_version = "0.1.0";
        journal.state = u::TransactionState::Prepared;
        journal.registration.key_path = "Software\\OpenWhisper_CPP\\Uninstall";
        journal.runonce_key = kRunOnce;
        context.expected_registration = journal.registration;
        context.expected_runonce_key = journal.runonce_key;
        fs::create_directories(paths.transaction_dir);
        make_tree(paths.install_dir, "old");
        make_tree(paths.candidate_dir, "new");
        u::save_journal(context, journal);
    }
    static void make_tree(const fs::path& path, std::string_view value) {
        fs::create_directories(path);
        fv::write_file_atomic(path / "version.txt", value);
    }
};
} // namespace fv::update_native::test
