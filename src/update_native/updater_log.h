#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "base/env.h"

// The updater's own log (services/app_update_apply.py: updater_log_path, setup_updater_logging).
// The helper has no window: without this file a failed update leaves one message box and no
// diagnosis.
namespace fv::update_native {

// RotatingFileHandler(maxBytes=512 * 1024, backupCount=1): updater.log and updater.log.1.
inline constexpr std::uintmax_t kUpdaterLogMaxBytes = 512 * 1024;
inline constexpr int kUpdaterLogBackupCount = 1;

// <local app dir>/updater.log: beside the application's own log, the parent of updates/
// (identity().data_dir_name under %LOCALAPPDATA%). FIORIVOCE_DATA_DIR does not move it, as it
// does not move updates/. Empty when the folder cannot be resolved.
std::filesystem::path updater_log_path(const EnvReader&);

// While alive, every log line at INFO or above goes to `file` (created with its folder), in the
// application log's format. The root level is INFO meanwhile and is restored after. A log that
// cannot be opened is no reason for an update to fail: the object is then inactive and nothing
// is thrown. Destroying it writes out what is queued (at most 1 s) and removes the sink.
// Affinity: one per process at a time, made and destroyed on one thread.
class UpdaterLog {
public:
    explicit UpdaterLog(const std::filesystem::path& file);
    ~UpdaterLog();
    UpdaterLog(const UpdaterLog&) = delete;
    UpdaterLog& operator=(const UpdaterLog&) = delete;

    // The file is open and receives the lines.
    bool active() const;

    struct Parts;

private:
    std::unique_ptr<Parts> parts_;
};

} // namespace fv::update_native
