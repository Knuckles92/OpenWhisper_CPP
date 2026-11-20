// Replays tests/golden/services.diagnostics.json (tools/parity/cases/services.diagnostics.py).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <functional>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

#include "golden/golden.h"
#include "log/failure_journal.h"
#include "log/metrics_capture.h"
#include "log/writer.h"
#include "log_test_support.h"
#include "pyport/errors.h"
#include "pyport/json.h"

using fv::Json;
namespace flog = fv::log;

namespace {

constexpr std::string_view kModule = "services.diagnostics";

// Python's json.dumps of both sides: the same text means the same values with the same int and
// float types, which golden::equal (Python ==) would let differ.
void check_same(const Json& expected, const Json& actual) {
    CHECK(fv::py::dumps(fv::logtest::untag(expected)) == fv::py::dumps(actual));
}

struct KeyError : std::exception {
    const char* what() const noexcept override { return "KeyError"; }
};

// A C++ exception of the kind that stands in for each Python exception of the vectors.
void with_exception(const std::string& name,
                    const std::function<void(const std::exception*)>& use) {
    using std::errc;
    auto os = [&](errc code) {
        const std::system_error error(std::make_error_code(code));
        use(&error);
    };
    if (name == "TimeoutError") {
        os(errc::timed_out);
    } else if (name == "FileNotFoundError") {
        const std::filesystem::filesystem_error error(
            "x", std::make_error_code(errc::no_such_file_or_directory));
        use(&error);
    } else if (name == "PermissionError") {
        os(errc::permission_denied);
    } else if (name == "ConnectionResetError") {
        os(errc::connection_reset);
    } else if (name == "ConnectionRefusedError") {
        os(errc::connection_refused);
    } else if (name == "BrokenPipeError") {
        os(errc::broken_pipe);
    } else if (name == "ConnectionError") {
        os(errc::connection_aborted);
    } else if (name == "OSError") {
        os(errc::io_error);
    } else if (name == "MemoryError") {
        const std::bad_alloc error;
        use(&error);
    } else if (name == "ValueError") {
        const std::invalid_argument error("x");
        use(&error);
    } else if (name == "UnicodeDecodeError") {
        const fv::py::ValueError error("x");
        use(&error);
    } else if (name == "RuntimeError") {
        const std::runtime_error error("x");
        use(&error);
    } else if (name == "NotImplementedError") {
        const std::range_error error("x");
        use(&error);
    } else if (name == "RecursionError") {
        const fv::py::RecursionError error("x");
        use(&error);
    } else if (name == "KeyError") {
        const KeyError error;
        use(&error);
    } else if (name == "IndexError") {
        const fv::py::IndexError error("x");
        use(&error);
    } else if (name == "TypeError") {
        const fv::py::TypeError error("x");
        use(&error);
    } else if (name == "Exception") {
        const std::exception error;
        use(&error);
    } else {
        FAIL("no C++ stand-in for " << name);
    }
}

} // namespace

TEST_CASE("golden: _failure_events", "[golden][test_diagnostics]") {
    fv::golden::for_each(kModule, "_failure_events", [](const fv::golden::Case& c) {
        INFO(c.label());
        check_same(c.result<Json>(), flog::failure_events(fv::logtest::untag(c.arg<Json>(0))));
    });
}

TEST_CASE("golden: _metric_samples", "[golden][test_diagnostics]") {
    fv::golden::for_each(kModule, "_metric_samples", [](const fv::golden::Case& c) {
        INFO(c.label());
        check_same(c.result<Json>(), flog::metric_samples(fv::logtest::untag(c.arg<Json>(0))));
    });
}

TEST_CASE("golden: _metric_summary", "[golden][test_diagnostics]") {
    fv::golden::for_each(kModule, "_metric_summary", [](const fv::golden::Case& c) {
        INFO(c.label());
        check_same(c.result<Json>(), flog::metric_summary(fv::logtest::untag(c.arg<Json>(0))));
    });
}

// The vectors ran FailureCapture.handle on a LogRecord at line 44; here the record goes through
// FailureJournalSink, and the event it appends is compared without its time.
TEST_CASE("golden: FailureCapture component and category mapping", "[golden][test_diagnostics]") {
    fv::logtest::TempDir folder;
    fv::golden::for_each(kModule, "FailureCapture", [&](const fv::golden::Case& c) {
        INFO(c.label());
        const auto logger = c.arg<std::string>(0);
        const auto exception = c.arg<std::optional<std::string>>(1);
        flog::LogWriter writer;
        const auto path = folder.path() / "journal.json";
        std::filesystem::remove(path);
        flog::FailureJournalSink sink(path, writer);
        auto handle = [&](const std::exception* error) {
            flog::Record record{};
            record.level = flog::Level::Error;
            record.logger = logger;
            record.message = "message";
            record.line = 44;
            record.time = std::chrono::system_clock::now();
            record.exception = error;
            sink.write(record);
        };
        if (exception) {
            with_exception(*exception, handle);
        } else {
            handle(nullptr);
        }
        Json event = sink.events().back();
        event.erase("utc");
        check_same(c.result<Json>(), event);
        CHECK(std::string(flog::component_for_logger(logger)) ==
              c.result<Json>()["component"].get<std::string>());
    });
}
