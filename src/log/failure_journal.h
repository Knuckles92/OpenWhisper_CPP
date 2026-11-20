#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <memory>
#include <string_view>

#include "base/json.h"
#include "base/log.h"
#include "log/writer.h"

namespace fv::log {

// diagnostics.JOURNAL_NAME and MAX_FAILURES.
inline constexpr std::string_view kJournalName = "diagnostic-failures.json";
inline constexpr std::size_t kMaxFailures = 50;

// FailureCapture.emit's component for a dotted logger name: "recorder" for "services.recorder"
// and "services.recorder.x" (not "services.recorderx"), likewise the other ten known services;
// "transcriber", "ui" and "meeting" for the "transcriber.", "ui_qt." and "meeting." prefixes;
// "application" for anything else.
std::string_view component_for_logger(std::string_view logger);

// The Python class name FailureCapture records as the category, from the first of _EXCEPTIONS
// the exception is an instance of, with C++ types standing in for Python's:
//   std::bad_alloc                                   MemoryError
//   std::system_error (filesystem_error, ios failure) by its code: timed_out TimeoutError;
//       permission_denied, operation_not_permitted PermissionError; no_such_file_or_directory
//       FileNotFoundError; connection_aborted, connection_refused, connection_reset, broken_pipe
//       ConnectionError; anything else OSError
//   std::invalid_argument, std::domain_error, std::out_of_range, py::ValueError   ValueError
//   std::runtime_error (and py::RecursionError)      RuntimeError
//   anything else                                    Error
// nullptr (an error logged without an exception) is "LoggedError". Never calls what().
std::string_view category_for_exception(const std::exception* error);

// diagnostics._failure_events: the last 50 entries of a persisted journal, each kept only with
// an allowlisted component and category and a timestamp fromisoformat accepts (rewritten by
// isoformat); `line` stays when it is an int in 1..99999 and becomes null otherwise. Anything
// else in an entry (message text, a traceback) is dropped, so an edited journal cannot inject a
// payload into a report.
Json failure_events(const Json& value);

// FailureCapture: every ERROR or CRITICAL record appends {utc, component, category, line} to
// diagnostic-failures.json, keeping the last 50. The message and the exception's text are never
// read. The file is rewritten on fv-log (json.dumps(indent=2) through the ".tmp" sibling and a
// replace); storage errors are swallowed without logging, which would recurse.
// Affinity: thread-safe.
class FailureJournalSink final : public Sink {
public:
    // Loads and revalidates what the file already holds.
    FailureJournalSink(std::filesystem::path path, LogWriter& writer);
    ~FailureJournalSink() override;

    void write(const Record&) override;
    // Waits at most `budget` for the latest events to reach the file.
    void flush(std::chrono::milliseconds budget) override;

    // The events in memory, newest last.
    Json events() const;
    const std::filesystem::path& path() const;

    struct State;

private:
    std::shared_ptr<State> state_;
    LogQueue queue_;
    CoalescedJob job_;
};

} // namespace fv::log
