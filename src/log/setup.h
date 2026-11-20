#pragma once

#include <chrono>
#include <memory>

#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"

namespace fv::log {

class LogWriter;
class MetricsCapture;
class FailureJournalSink;

// What install_app_logging set up. Destroying it is logging.shutdown(): the metrics get at most
// 100 ms to land (FailureCapture.close), the queued log lines at most 1 s (Python wrote them
// synchronously; the bound keeps a stalled disk from holding up exit), then the sinks are removed
// and fv-log ends.
class LoggingHandle {
public:
    LoggingHandle();
    ~LoggingHandle();
    LoggingHandle(LoggingHandle&&) noexcept;
    LoggingHandle& operator=(LoggingHandle&&) noexcept;
    LoggingHandle(const LoggingHandle&) = delete;
    LoggingHandle& operator=(const LoggingHandle&) = delete;

    // Empty for a moved-from or default-made handle.
    MetricsCapture* metrics() const;
    FailureJournalSink* failures() const;
    LogWriter* writer() const;

    struct Parts;
    explicit LoggingHandle(std::unique_ptr<Parts> parts);

private:
    std::unique_ptr<Parts> parts_;
};

// bootstrap.setup_logging: the root level from <env_prefix>LOG_LEVEL (INFO by default), the
// rotating log file (identity's log_file in the data root, 5 MiB x 3, UTF-8), a console sink when
// `console` (a console is attached; a windowed build has none), "httpx" and "httpcore" held at
// WARNING because request URLs can carry dictated words or tokens, the failure journal and the
// metrics capture beside the log file, and the crash handler. Once per process. Throws
// std::filesystem::filesystem_error when the log file cannot be opened.
LoggingHandle install_app_logging(const Paths&, const AppIdentity&, const EnvReader&,
                                  bool console); // console: a console is attached

} // namespace fv::log
