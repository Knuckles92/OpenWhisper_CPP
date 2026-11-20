// The log facade, the metrics facade and the null event sink.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "base/events.h"
#include "base/log.h"
#include "base/metrics.h"
#include "base/testing/map_env_reader.h"

using fv::MapEnvReader;
using fv::log::Level;

namespace {

struct Captured {
    Level level;
    std::string logger;
    std::string message;
    std::string exception_type;
    std::uint32_t line;
    bool has_exception;
};

class CaptureSink final : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        records.push_back({record.level, std::string(record.logger), std::string(record.message),
                           std::string(record.exception_type), record.line,
                           record.exception != nullptr});
        CHECK(record.time.time_since_epoch().count() != 0);
    }
    void flush(std::chrono::milliseconds) override { ++flushes; }

    std::vector<Captured> records;
    int flushes = 0;
};

class ThrowingSink final : public fv::log::Sink {
public:
    void write(const fv::log::Record&) override { throw std::runtime_error("sink failed"); }
};

// Installs a capture sink for one test and puts the facade back as it was.
struct Installed {
    std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();
    Installed() {
        fv::log::set_level(Level::Info);
        fv::log::add_sink(sink);
    }
    ~Installed() {
        fv::log::remove_sink(sink);
        fv::log::set_level(Level::Info);
    }
};

constexpr fv::log::Logger kRecorder{"services.recorder"};

} // namespace

TEST_CASE("a log call reaches the sinks formatted, with its logger, level and line") {
    Installed installed;
    const std::uint32_t line = __LINE__ + 1;
    kRecorder.info("Recording started on device {} at {} Hz", 3, 44100);

    REQUIRE(installed.sink->records.size() == 1);
    const auto& record = installed.sink->records[0];
    CHECK(record.level == Level::Info);
    CHECK(record.logger == "services.recorder");
    CHECK(record.message == "Recording started on device 3 at 44100 Hz");
    CHECK(record.line == line);
    CHECK(record.exception_type.empty());
    CHECK_FALSE(record.has_exception);
}

TEST_CASE("records below the level are dropped before formatting") {
    Installed installed;
    kRecorder.debug("hidden {}", 1);
    fv::log::set_level(Level::Warning);
    kRecorder.info("hidden too");
    kRecorder.warning("shown");
    kRecorder.critical("shown {}", "as well");
    REQUIRE(installed.sink->records.size() == 2);
    CHECK(installed.sink->records[0].message == "shown");
    CHECK(installed.sink->records[1].level == Level::Critical);
}

TEST_CASE("a logger's own level covers its children, as logging.getLogger(name).setLevel") {
    Installed installed;
    fv::log::set_logger_level("httpx", Level::Warning);
    constexpr fv::log::Logger httpx{"httpx"};
    constexpr fv::log::Logger client{"httpx.client"};
    constexpr fv::log::Logger other{"httpxy"};
    httpx.info("GET with a query");
    client.info("GET with a query");
    other.info("kept");
    client.error("failed");
    fv::log::set_logger_level("httpx", std::nullopt);
    httpx.info("kept again");

    std::vector<std::string> messages;
    for (const auto& record : installed.sink->records) {
        messages.push_back(record.logger + ": " + record.message);
    }
    CHECK(messages ==
          std::vector<std::string>{"httpxy: kept", "httpx.client: failed", "httpx: kept again"});
}

TEST_CASE("error_with carries the exception's type and the message as given") {
    Installed installed;
    try {
        throw std::invalid_argument("secret dictated words");
    } catch (const std::exception& error) {
        kRecorder.error_with(error, "Could not save the recording");
    }
    REQUIRE(installed.sink->records.size() == 1);
    const auto& record = installed.sink->records[0];
    CHECK(record.level == Level::Error);
    CHECK(record.message == "Could not save the recording");
    CHECK(record.exception_type == "std::invalid_argument");
    CHECK(record.has_exception);
}

TEST_CASE("a failing sink does not stop the next one, and removed sinks hear nothing") {
    auto throwing = std::make_shared<ThrowingSink>();
    fv::log::add_sink(throwing);
    {
        Installed installed;
        kRecorder.warning("still delivered");
        CHECK(installed.sink->records.size() == 1);
        fv::log::flush(std::chrono::milliseconds(50));
        CHECK(installed.sink->flushes == 1);
    }
    fv::log::remove_sink(throwing);
    Installed later;
    fv::log::remove_sink(later.sink);
    kRecorder.warning("nobody listens");
    CHECK(later.sink->records.empty());
}

TEST_CASE("the log level comes from the environment as Python's getattr(logging, name)") {
    const auto& app = fv::identity();
    const auto level = [&](const char* value) {
        return fv::log::level_from_env(MapEnvReader{{"FIORIVOCE_LOG_LEVEL", value}}, app);
    };
    CHECK(fv::log::level_from_env(MapEnvReader{}, app) == Level::Info);
    CHECK(level("debug") == Level::Debug);
    CHECK(level("NOTSET") == Level::Debug);
    CHECK(level("Info") == Level::Info);
    CHECK(level("warn") == Level::Warning);
    CHECK(level("WARNING") == Level::Warning);
    CHECK(level("error") == Level::Error);
    CHECK(level("fatal") == Level::Critical);
    CHECK(level("CRITICAL") == Level::Critical);
    CHECK(level("verbose") == Level::Info);
    CHECK(level(" debug") == Level::Info);      // no stripping in Python either
    CHECK(level("\xC4\xB1nfo") == Level::Info); // "ınfo".upper() is "INFO"
    CHECK(level("") == Level::Info);
    CHECK(fv::log::level_from_env(MapEnvReader{{"OPENWHISPER_LOG_LEVEL", "DEBUG"}}, app) ==
          Level::Info);
}

TEST_CASE("metrics go to the installed sink and nowhere before it") {
    fv::metrics::record("hotkey_to_record_ms", 12.5); // no sink: a no-op
    std::vector<std::pair<std::string, double>> seen;
    fv::metrics::set_sink(
        [&](std::string_view name, double value) { seen.emplace_back(std::string(name), value); });
    fv::metrics::record("hotkey_to_record_ms", 12.5);
    fv::metrics::record("paste_ms", 3.0);
    fv::metrics::set_sink({});
    fv::metrics::record("paste_ms", 4.0);
    CHECK(seen == std::vector<std::pair<std::string, double>>{{"hotkey_to_record_ms", 12.5},
                                                              {"paste_ms", 3.0}});
}

TEST_CASE("the null event sink takes every event") {
    auto& sink = fv::null_event_sink();
    sink.publish("transcription.completed", fv::Json{{"text_length", 12}});
    sink.publish("preview.updated", fv::Json::object(), false);
    CHECK(&sink == &fv::null_event_sink());
}
