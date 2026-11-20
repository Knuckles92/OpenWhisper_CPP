#include "log/setup.h"

#include <algorithm>
#include <string>
#include <utility>

#include "base/log.h"
#include "base/utf8.h"
#include "log/console_sink.h"
#include "log/crash_handler.h"
#include "log/failure_journal.h"
#include "log/metrics_capture.h"
#include "log/rotating_file_sink.h"
#include "log/writer.h"

namespace fv::log {

namespace {

// logging.info and logging.warning in bootstrap.py go to the root logger.
constexpr Logger kRoot{"root"};

// FailureCapture.close gives the last metric this long; MetricsCapture.close's default.
constexpr std::chrono::milliseconds kMetricsCloseBudget{100};
constexpr std::chrono::milliseconds kLogFlushBudget{1000};

} // namespace

struct LoggingHandle::Parts {
    std::unique_ptr<LogWriter> writer;
    std::shared_ptr<RotatingFileSink> file;
    std::shared_ptr<ConsoleSink> console;
    std::shared_ptr<FailureJournalSink> failures;
    std::unique_ptr<MetricsCapture> metrics;

    ~Parts() {
        if (metrics) {
            metrics->uninstall();
            metrics->close(kMetricsCloseBudget);
        }
        const auto deadline = std::chrono::steady_clock::now() + kLogFlushBudget;
        for (const std::shared_ptr<Sink>& sink :
             {std::shared_ptr<Sink>(file), std::shared_ptr<Sink>(console),
              std::shared_ptr<Sink>(failures)}) {
            if (sink) {
                remove_sink(sink);
            }
        }
        if (writer) {
            writer->stop(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::max(deadline - std::chrono::steady_clock::now(),
                         std::chrono::steady_clock::duration::zero())));
        }
    }
};

LoggingHandle::LoggingHandle() = default;
LoggingHandle::LoggingHandle(std::unique_ptr<Parts> parts) : parts_(std::move(parts)) {}
LoggingHandle::~LoggingHandle() = default;
LoggingHandle::LoggingHandle(LoggingHandle&&) noexcept = default;
LoggingHandle& LoggingHandle::operator=(LoggingHandle&&) noexcept = default;

MetricsCapture* LoggingHandle::metrics() const {
    return parts_ ? parts_->metrics.get() : nullptr;
}

FailureJournalSink* LoggingHandle::failures() const {
    return parts_ ? parts_->failures.get() : nullptr;
}

LogWriter* LoggingHandle::writer() const {
    return parts_ ? parts_->writer.get() : nullptr;
}

LoggingHandle install_app_logging(const Paths& paths, const AppIdentity& identity,
                                  const EnvReader& env, bool console) {
    set_level(level_from_env(env, identity));

    auto parts = std::make_unique<LoggingHandle::Parts>();
    parts->writer = std::make_unique<LogWriter>();
    const auto log_file = paths.user_file(identity.log_file);
    parts->file = std::make_shared<RotatingFileSink>(log_file, *parts->writer);
    add_sink(parts->file);
    if (console) {
        parts->console = std::make_shared<ConsoleSink>(*parts->writer);
        add_sink(parts->console);
    }
    // httpx and httpcore log every request's full URL at INFO, and URLs can carry dictated words
    // (link actions), tokens or webhook paths.
    for (const char* name : {"httpx", "httpcore"}) {
        set_logger_level(name, Level::Warning);
    }

    // diagnostics.install_failure_capture: both files live beside the log file.
    const auto folder = log_file.parent_path();
    parts->failures =
        std::make_shared<FailureJournalSink>(folder / std::string(kJournalName), *parts->writer);
    add_sink(parts->failures);
    parts->metrics =
        std::make_unique<MetricsCapture>(folder / std::string(kMetricsName), *parts->writer);
    parts->metrics->install();

    try {
        CrashHandler::install(paths, identity);
        kRoot.info("Crash handler enabled for crash diagnostics: {}",
                   path_to_utf8(CrashHandler::crash_log_file(paths, identity)));
    } catch (const std::exception& error) {
        kRoot.warning("Failed to enable the crash handler: {}", error.what());
    }
    return LoggingHandle(std::move(parts));
}

} // namespace fv::log
