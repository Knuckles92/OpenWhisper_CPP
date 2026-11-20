#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "backup/coordinator.h"

// Fakes for BackupCoordinator: scripted reservation hooks and an archive service that writes
// placeholder files, as OpenWhisper's tests/test_backup_manager.py builds them.
namespace fv::backup::testing {

// Hooks that record "acquire", "release", "context_enter" and "context_exit" in order and refuse
// the reservation with `refusal` while it is set.
// Affinity: thread-safe.
class FakeHooks {
public:
    BackupCoordinator::Hooks hooks();

    void set_refusal(std::optional<std::string> refusal);
    std::vector<std::string> events() const;
    void record(std::string event);

private:
    struct State {
        mutable std::mutex mutex;
        std::vector<std::string> events;
        std::optional<std::string> refusal;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

// WritingService: create_backup writes "archive" to the destination and reports one progress
// step ("Writing archive", 7, 7); inspect_backup and prepare_restore return the archive itself.
// Each call is recorded as {"backup", path, "true"|"false"}, {"inspect", path} or {"restore",
// path}. The optional hooks run first, on the worker, with the call's arguments.
// Affinity: thread-safe.
class FakeBackupService final : public BackupService {
public:
    using Call = std::vector<std::string>;

    BackupInfo create_backup(const std::filesystem::path& destination,
                             const CreateOptions& options) override;
    BackupInfo inspect_backup(const std::filesystem::path& archive) override;
    std::filesystem::path prepare_restore(const std::filesystem::path& archive,
                                          const std::filesystem::path& root) override;
    std::optional<std::filesystem::path>
    previous_data_dir(const std::filesystem::path& root) override;
    // Recorded as {"import", source, "true"|"false" (copy_credentials)}; on_import's answer, or
    // the source folder as the stage.
    ImportResult prepare_import(const std::filesystem::path& source,
                                const std::filesystem::path& root,
                                const ImportOptions& options) override;

    std::vector<Call> calls() const;

    // Runs inside prepare_import, on the worker; it may block or throw (an OpenWhisperRunning
    // refusal).
    std::function<ImportResult(const std::filesystem::path& source, const ImportOptions&)>
        on_import;

    // Runs inside create_backup before it writes; it may block, throw or watch the cancel token.
    std::function<void(const std::filesystem::path&, const CreateOptions&)> on_create;
    // previous_data_dir's answer; it may throw. Without it, nullopt.
    std::function<std::optional<std::filesystem::path>(const std::filesystem::path& root)>
        on_previous;

    // _info(path): created 2026-10-04T12:00:00+00:00, app 2.5.0, schema 1, recordings, 2 files,
    // 20 bytes, the file's size.
    static BackupInfo info_for(const std::filesystem::path& path);

private:
    mutable std::mutex mutex_;
    std::vector<Call> calls_;
};

} // namespace fv::backup::testing
