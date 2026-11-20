#pragma once

#include <chrono>
#include <functional>
#include <string_view>

#include "base/log.h"
#include "log/writer.h"

namespace fv::log {

// logging.StreamHandler(): every record, in config.LOG_FORMAT, to standard error. The write
// happens on fv-log, so a blocked console or pipe never stalls the caller. Installed only when
// the process has a console (setup_logging skips it when sys.stdout is None).
// Affinity: thread-safe.
class ConsoleSink final : public Sink {
public:
    // `out` receives each line with its "\n"; the default writes to standard error (UTF-16 to a
    // Windows console). `zone` nullptr is the system's zone.
    explicit ConsoleSink(LogWriter& writer, std::function<void(std::string_view)> out = {},
                         const std::chrono::time_zone* zone = nullptr);

    void write(const Record&) override;
    void flush(std::chrono::milliseconds budget) override;

private:
    LogQueue queue_;
    std::function<void(std::string_view)> out_;
    const std::chrono::time_zone* zone_;
};

} // namespace fv::log
