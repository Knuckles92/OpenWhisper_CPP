#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "base/log.h"
#include "log/writer.h"

namespace fv::log {

// config.LOG_MAX_BYTES and config.LOG_BACKUP_COUNT: openwhisper-cpp.log plus .1 to .3.
inline constexpr std::uintmax_t kLogMaxBytes = 5 * 1024 * 1024;
inline constexpr int kLogBackupCount = 3;

// Python's logging level names.
std::string_view level_name(Level);

// config.LOG_FORMAT, "%(asctime)s - %(levelname)s - %(name)s - %(message)s", with asctime
// "YYYY-MM-DD HH:MM:SS,mmm" in `zone` (milliseconds truncated, as logging computes msecs).
std::string format_record(const Record&, const std::chrono::time_zone* zone);

// logging.handlers.RotatingFileHandler(maxBytes, backupCount, encoding="utf-8") behind the
// writer thread: the line is formatted on the caller's thread and written on fv-log. Before a
// line goes into a non-empty file, the file rolls over when its size in bytes plus the line's
// length in code points (with its "\n") reaches max_bytes, as Python compares tell() with
// len(msg). Lines end as Python's text mode ends them ("\r\n" on Windows).
// Affinity: thread-safe.
class RotatingFileSink final : public Sink {
public:
    // Opens (creates) the file now, as FileHandler does without delay; throws
    // std::filesystem::filesystem_error when it cannot. `zone` nullptr is the system's zone.
    RotatingFileSink(std::filesystem::path file, LogWriter& writer,
                     std::uintmax_t max_bytes = kLogMaxBytes, int backup_count = kLogBackupCount,
                     const std::chrono::time_zone* zone = nullptr);
    ~RotatingFileSink() override;

    void write(const Record&) override;
    void flush(std::chrono::milliseconds budget) override;

    const std::filesystem::path& file() const;

    struct State;

private:
    std::shared_ptr<State> state_;
    LogQueue queue_;
    const std::chrono::time_zone* zone_;
};

} // namespace fv::log
