#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "update/install_probe.h"

namespace fv::update_native {

enum class TransactionState {
    Preparing,
    Prepared,
    OldMoved,
    NewActive,
    Healthy,
    RolledBack,
    Superseded
};

// These roots come from the running installation and the app's own data configuration, never
// from a journal. Both must be absolute; install_dir names the installed application directory.
struct JournalContext {
    std::filesystem::path data_root;
    std::filesystem::path install_dir;
    std::string app_id;
    // The already identified parent executable, when a parent PID is recorded. It is supplied
    // independently of the journal and must be directly inside install_dir.
    std::filesystem::path parent_exe;
    // Discovered or derived independently of the journal. Mutation code must not use a
    // registration or RunOnce target selected by the journal itself.
    std::optional<update::InstallRegistration> expected_registration;
    std::string expected_runonce_key;
};

// Derived from JournalContext and a validated transaction ID. Transaction code must use these
// paths, not paths read from an untrusted journal.
struct JournalPaths {
    std::filesystem::path data_root;
    std::filesystem::path updates_root;
    std::filesystem::path transaction_dir;
    std::filesystem::path journal_file;
    std::filesystem::path install_dir;
    std::filesystem::path candidate_dir;
    std::filesystem::path rollback_dir;
    // Where a new tree that did not become healthy is moved before the old one returns.
    std::filesystem::path failed_dir;
    std::filesystem::path archive_file;
    std::filesystem::path parent_exe;
};

struct UpdateJournal {
    std::string transaction_id;
    TransactionState state = TransactionState::Preparing;
    std::filesystem::path app_dir, candidate_dir, rollback_dir;
    std::string old_version, new_version, old_display_version;
    std::optional<std::int64_t> old_estimated_size_kb;
    std::string old_install_date, old_exe_sha256, new_exe_sha256, old_manifest_sha256;
    std::uint32_t parent_pid = 0;
    std::filesystem::path parent_exe;
    std::int64_t parent_creation_time = 0;
    std::string health_token;
    std::vector<std::string> uninstaller_files;
    Json compatibility_files = Json::object();
    std::filesystem::path appdata, archive_path;
    bool cleanup_rollback = false;
    update::InstallRegistration registration;
    std::string runonce_key;
    std::string app_id;
};

std::string_view to_string(TransactionState state);
TransactionState parse_transaction_state(std::string_view state);
JournalPaths journal_paths(const JournalContext& context, std::string_view transaction_id);
// Initializes identity, paths, state and a random health token; callers fill release metadata
// and the independently discovered registration before saving.
UpdateJournal make_journal(const JournalContext& context, std::string_view transaction_id);
Json to_json(const UpdateJournal& journal);
UpdateJournal journal_from_json(const Json& payload);
bool is_test_journal(const UpdateJournal& journal);
// Validates all fields and returns paths derived only from context and transaction_id.
JournalPaths validate_journal_binding(const JournalContext& context, const UpdateJournal& journal,
                                      std::string_view transaction_id);
// The transaction directory must already exist. Writes are atomic and flushed; reads reject
// links, hard links, non-regular files and oversized input. Errors are update_native::UpdateError.
void save_journal(const JournalContext& context, const UpdateJournal& journal);
UpdateJournal load_journal(const JournalContext& context, std::string_view transaction_id);
} // namespace fv::update_native
