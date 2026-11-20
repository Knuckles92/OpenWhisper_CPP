#include "backup/testing/fake_hooks.h"

#include <utility>

#include "backup/detail/fs.h"
#include "backup/testing/sample_data.h"
#include "base/utf8.h"

namespace fv::backup::testing {

BackupCoordinator::Hooks FakeHooks::hooks() {
    BackupCoordinator::Hooks hooks;
    std::weak_ptr<State> weak = state_;
    hooks.acquire = [weak]() -> std::optional<std::string> {
        const auto state = weak.lock();
        if (!state) {
            return std::nullopt;
        }
        std::lock_guard lock(state->mutex);
        state->events.emplace_back("acquire");
        return state->refusal;
    };
    hooks.release = [weak] {
        if (const auto state = weak.lock()) {
            std::lock_guard lock(state->mutex);
            state->events.emplace_back("release");
        }
    };
    hooks.worker_context = [weak](const std::function<void()>& body) {
        const auto record = [&weak](const char* event) {
            if (const auto state = weak.lock()) {
                std::lock_guard lock(state->mutex);
                state->events.emplace_back(event);
            }
        };
        record("context_enter");
        try {
            body();
        } catch (...) {
            record("context_exit");
            throw;
        }
        record("context_exit");
    };
    return hooks;
}

void FakeHooks::set_refusal(std::optional<std::string> refusal) {
    std::lock_guard lock(state_->mutex);
    state_->refusal = std::move(refusal);
}

std::vector<std::string> FakeHooks::events() const {
    std::lock_guard lock(state_->mutex);
    return state_->events;
}

void FakeHooks::record(std::string event) {
    std::lock_guard lock(state_->mutex);
    state_->events.push_back(std::move(event));
}

BackupInfo FakeBackupService::info_for(const std::filesystem::path& path) {
    BackupInfo info;
    info.path = path;
    info.created_at = "2026-10-04T12:00:00+00:00";
    info.app_version = "2.5.0";
    info.schema_version = 1;
    info.include_recordings = true;
    info.file_count = 2;
    info.total_bytes = 20;
    info.archive_bytes = detail::stat(path).size;
    return info;
}

BackupInfo FakeBackupService::create_backup(const std::filesystem::path& destination,
                                            const CreateOptions& options) {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back(
            {"backup", path_to_utf8(destination), options.include_recordings ? "true" : "false"});
    }
    if (on_create) {
        on_create(destination, options);
    }
    if (options.cancel.cancelled()) {
        throw BackupCancelled();
    }
    write_bytes(destination, "archive");
    if (options.progress) {
        options.progress("Writing archive", 7, 7);
    }
    return info_for(destination);
}

BackupInfo FakeBackupService::inspect_backup(const std::filesystem::path& archive) {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back({"inspect", path_to_utf8(archive)});
    }
    return info_for(archive);
}

std::filesystem::path FakeBackupService::prepare_restore(const std::filesystem::path& archive,
                                                         const std::filesystem::path&) {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back({"restore", path_to_utf8(archive)});
    }
    return archive;
}

std::optional<std::filesystem::path>
FakeBackupService::previous_data_dir(const std::filesystem::path& root) {
    return on_previous ? on_previous(root) : std::nullopt;
}

ImportResult FakeBackupService::prepare_import(const std::filesystem::path& source,
                                               const std::filesystem::path&,
                                               const ImportOptions& options) {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back(
            {"import", path_to_utf8(source), options.copy_credentials ? "true" : "false"});
    }
    if (on_import) {
        return on_import(source, options);
    }
    ImportResult result;
    result.stage = source;
    return result;
}

std::vector<FakeBackupService::Call> FakeBackupService::calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
}

} // namespace fv::backup::testing
