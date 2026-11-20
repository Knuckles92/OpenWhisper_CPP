// A support report must remain useful without exporting private user content.
// from tests/test_diagnostics.py

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "base/identity.h"
#include "base/metrics.h"
#include "log/diagnostics_report.h"
#include "log/failure_journal.h"
#include "log/metrics_capture.h"
#include "log/writer.h"
#include "log_test_support.h"
#include "pyport/json.h"

using fv::Json;
using fv::logtest::read_file;
using fv::logtest::TempDir;
using fv::logtest::write_file;
namespace flog = fv::log;

namespace {

constexpr const char* kPrivate =
    "secret transcript sk-private-key user@example.com C:/Private/recording.wav";

flog::ReportInputs report_inputs(const std::filesystem::path& root, const Json* settings) {
    flog::ReportInputs inputs;
    inputs.app_version = "0.1.0";
    inputs.settings = settings;
    inputs.data_root = root;
    inputs.components = {{"qt", "6.8.3"},
                         {"nemo_speech_runtime", std::nullopt},
                         {"sqlite", "3.47.0"},
                         {"openssl", "3.6.5"}};
    inputs.nvidia_gpu = [] {
        return std::optional<flog::GpuInfo>();
    };
    return inputs;
}

flog::Record error_record(std::string_view logger, std::string_view message,
                          const std::exception* error, std::uint32_t line) {
    flog::Record record{};
    record.level = flog::Level::Error;
    record.logger = logger;
    record.message = message;
    record.line = line;
    record.time = std::chrono::system_clock::now();
    record.exception = error;
    record.file = "private/path";
    return record;
}

} // namespace

// from tests/test_diagnostics.py::test_export_allowlists_settings_and_failure_fields
// Adapted to the Parakeet engine block: selected_model's allowlist is parakeet, api and remote;
// the Whisper settings always report default_or_custom; packages became components.
TEST_CASE("export allowlists settings and failure fields", "[test_diagnostics]") {
    TempDir root;
    Json settings = Json::object();
    settings["selected_model"] = "parakeet";
    settings["whisper_device"] = "cpu";
    settings["whisper_model"] = kPrivate;
    settings["api_key"] = kPrivate;
    settings["remote_pairings"] = Json::array({kPrivate});
    settings["microphone"] = kPrivate;
    settings["local_asr_models"] = Json::object({{"nemo", kPrivate}});
    Json journal = Json::array();
    journal.push_back({{"utc", "2026-01-01T00:00:00+00:00"},
                       {"component", "recorder"},
                       {"category", "OSError"},
                       {"line", 123},
                       {"message", kPrivate},
                       {"traceback", kPrivate}});
    journal.push_back(
        {{"utc", kPrivate}, {"component", "recorder"}, {"category", "OSError"}, {"line", 123}});
    journal.push_back(
        {{"utc", "2026-01-01"}, {"component", kPrivate}, {"category", "Error"}, {"line", 123}});
    write_file(root.path() / std::string(flog::kJournalName), fv::py::dumps(journal));

    const Json report = flog::collect_report(report_inputs(root.path(), &settings));
    const std::string serialized = fv::py::dumps(report);

    CHECK(serialized.find(kPrivate) == std::string::npos);
    CHECK(report["engine"]["selected_model"] == "parakeet");
    CHECK(report["engine"]["whisper_model"] == "default_or_custom");
    CHECK(report["engine"]["whisper_device"] == "default_or_custom");
    CHECK(report["engine"]["local_models"] == Json::array());
    const Json expected = Json::array({Json{{"utc", "2026-01-01T00:00:00+00:00"},
                                            {"component", "recorder"},
                                            {"category", "OSError"},
                                            {"line", 123}}});
    CHECK(report["recent_failures"] == expected);
    CHECK(report.contains("app_version"));
    CHECK(report["components"].contains("nemo_speech_runtime"));
    CHECK(report["components"]["nemo_speech_runtime"].is_null());
    CHECK(report["platform"]["nvidia_gpu"].is_null());
    CHECK(report["privacy"] == "No transcript, audio, log messages, paths, credentials, "
                               "hostnames or pairing data included.");
}

TEST_CASE("the report keeps only catalog ids among the local models, sorted and unique",
          "[test_diagnostics]") {
    TempDir root;
    Json settings = Json::object();
    settings["selected_model"] = "local_whisper";
    settings["local_asr_models"] = Json::object({{"a", "parakeet-tdt-0.6b-v3"},
                                                 {"b", kPrivate},
                                                 {"c", "parakeet-tdt-0.6b-v2"},
                                                 {"d", "parakeet-tdt-0.6b-v3"},
                                                 {"e", 3}});
    auto inputs = report_inputs(root.path(), &settings);
    inputs.catalog_model = [](std::string_view id) {
        return id.starts_with("parakeet-tdt-");
    };
    inputs.nvidia_gpu = [] {
        return std::optional<flog::GpuInfo>(
            flog::GpuInfo{"NVIDIA GeForce RTX 4090", 24564, std::pair{8, 9}});
    };
    const Json report = flog::collect_report(inputs);
    CHECK(report["engine"]["selected_model"] == "default_or_custom");
    CHECK(report["engine"]["local_models"] ==
          Json::array({"parakeet-tdt-0.6b-v2", "parakeet-tdt-0.6b-v3"}));
    CHECK(report["platform"]["nvidia_gpu"] == Json{{"name", "NVIDIA GeForce RTX 4090"},
                                                   {"total_mib", 24564},
                                                   {"compute_capability", {8, 9}}});
    // A card NVML gave no capability for: null, as Python's None.
    inputs.nvidia_gpu = [] {
        return std::optional<flog::GpuInfo>(flog::GpuInfo{"NVIDIA GeForce RTX 4090", 24564, {}});
    };
    CHECK(flog::collect_report(inputs)["platform"]["nvidia_gpu"]["compute_capability"] == nullptr);
    CHECK(flog::collect_report(inputs)["platform"]["nvidia_gpu"].dump() ==
          R"({"name":"NVIDIA GeForce RTX 4090","total_mib":24564,"compute_capability":null})");

    // A local_asr_models that is not an object leaves local_models out, as Python does.
    settings["local_asr_models"] = Json::array();
    CHECK_FALSE(flog::collect_report(inputs)["engine"].contains("local_models"));
    // Without settings local_asr_models is missing, which Python reads as {}.
    CHECK(flog::collect_report(report_inputs(root.path(), nullptr))["engine"]["local_models"] ==
          Json::array());
}

TEST_CASE("the report has schema 1's keys in Python's order", "[test_diagnostics]") {
    TempDir root;
    const Json report = flog::collect_report(report_inputs(root.path(), nullptr));
    std::vector<std::string> keys;
    for (const auto& [key, value] : report.items()) {
        keys.push_back(key);
    }
    CHECK(keys == std::vector<std::string>{"schema", "app_version", "packaged", "generated_utc",
                                           "platform", "engine", "components", "recent_failures",
                                           "recent_metrics", "privacy"});
    std::vector<std::string> platform;
    for (const auto& [key, value] : report["platform"].items()) {
        platform.push_back(key);
    }
    CHECK(platform ==
          std::vector<std::string>{"system", "release", "machine", "cpu_count", "nvidia_gpu"});
    CHECK(report["schema"] == 1);
    CHECK(report["packaged"] == false);
#if defined(_WIN32)
    CHECK(report["platform"]["system"] == "Windows");
#endif
    CHECK(report["platform"]["cpu_count"].get<std::int64_t>() > 0);
    std::vector<std::string> components;
    for (const auto& [key, value] : report["components"].items()) {
        components.push_back(key);
    }
    CHECK(components == std::vector<std::string>{"qt", "nemo_speech_runtime", "sqlite", "openssl"});
}

// from tests/test_diagnostics.py::test_failure_handler_never_formats_message_or_exception
TEST_CASE("failure handler never formats message or exception", "[test_diagnostics]") {
    static std::atomic<bool> inspected{false};
    class PrivateException : public std::invalid_argument {
    public:
        PrivateException() : std::invalid_argument("private") {}
        const char* what() const noexcept override {
            inspected = true;
            return "private";
        }
    };
    inspected = false;
    TempDir folder;
    flog::LogWriter writer;
    flog::FailureJournalSink handler(folder.path() / "journal.json", writer);
    const PrivateException error;
    const std::string message = "private message";
    for (int i = 0; i < 60; ++i) {
        handler.write(error_record("services.recorder", message, &error, 44));
    }
    REQUIRE(writer.flush(std::chrono::seconds(5)));
    const std::string text = read_file(handler.path());
    const Json events = fv::py::loads(text);
    CHECK(events.size() == flog::kMaxFailures);
    CHECK(events.back()["category"] == "ValueError");
    CHECK(events.back()["component"] == "recorder");
    CHECK(events.back()["line"] == 44);
    CHECK(text.find("private") == std::string::npos);
    CHECK_FALSE(inspected);
}

TEST_CASE("the journal is written as json.dumps(indent=2) in text mode, below ERROR ignored",
          "[test_diagnostics]") {
    TempDir folder;
    flog::LogWriter writer;
    flog::FailureJournalSink handler(folder.path() / std::string(flog::kJournalName), writer);
    auto warning = error_record("ui_qt.main_window", "x", nullptr, 7);
    warning.level = flog::Level::Warning;
    handler.write(warning);
    auto critical = error_record("ui_qt.main_window", "x", nullptr, 7);
    critical.level = flog::Level::Critical;
    critical.time = std::chrono::sys_days{std::chrono::year{2026} / 11 / 14} +
                    std::chrono::hours(10) + std::chrono::minutes(20) + std::chrono::seconds(30);
    handler.write(critical);
    REQUIRE(writer.flush(std::chrono::seconds(5)));
    const std::string expected =
        "[\n  {\n    \"utc\": \"2026-11-14T10:20:30+00:00\",\n    \"component\": \"ui\",\n"
        "    \"category\": \"LoggedError\",\n    \"line\": 7\n  }\n]";
#if defined(_WIN32)
    std::string bytes;
    for (const char c : expected) {
        if (c == '\n') {
            bytes += '\r';
        }
        bytes += c;
    }
#else
    const std::string bytes = expected;
#endif
    CHECK(read_file(handler.path()) == bytes);
    CHECK_FALSE(std::filesystem::exists(folder.path() / "diagnostic-failures.tmp"));

    // A second sink reads the journal back.
    flog::FailureJournalSink reopened(handler.path(), writer);
    CHECK(reopened.events() == handler.events());
}

// from tests/test_diagnostics.py::test_diagnostic_write_failure_does_not_replace_original_failure
TEST_CASE("diagnostic write failure does not replace original failure", "[test_diagnostics]") {
    TempDir folder;
    // A folder where the journal should be: the replace fails, as os.replace raising does.
    const auto path = folder.path() / "journal.json";
    std::filesystem::create_directories(path / "occupied");
    flog::LogWriter writer;
    flog::FailureJournalSink handler(path, writer);
    handler.write(error_record("services.recorder", "private", nullptr, 1));
    REQUIRE(writer.flush(std::chrono::seconds(5)));
    CHECK(handler.events().back()["category"] == "LoggedError");
    CHECK(std::filesystem::is_directory(path));
}

// from tests/test_diagnostics.py::test_export_never_overwrites_existing_file
TEST_CASE("export never overwrites existing file", "[test_diagnostics]") {
    TempDir folder;
    const auto output = folder.path() / "keep.json";
    write_file(output, "keep");
    try {
        flog::write_report_new(output, Json{{"schema", 1}});
        FAIL("write_report_new replaced an existing file");
    } catch (const std::filesystem::filesystem_error& error) {
        CHECK(error.code() == std::errc::file_exists);
    }
    CHECK(read_file(output) == "keep");
}

TEST_CASE("the export is json.dump(indent=2) and a newline, in text mode", "[test_diagnostics]") {
    TempDir folder;
    const auto output = folder.path() / "report.json";
    flog::write_report_new(output, Json{{"schema", 1}, {"engine", Json::object()}, {"x", 1.5}});
#if defined(_WIN32)
    CHECK(read_file(output) ==
          "{\r\n  \"schema\": 1,\r\n  \"engine\": {},\r\n  \"x\": 1.5\r\n}\r\n");
#else
    CHECK(read_file(output) == "{\n  \"schema\": 1,\n  \"engine\": {},\n  \"x\": 1.5\n}\n");
#endif
}

// from tests/test_diagnostics.py::test_metric_sample_bounds_and_export_statistics
// Python also records captured_frames=True, refused as a bool; the C++ facade takes only doubles,
// so a bool cannot reach it.
TEST_CASE("metric sample bounds and export statistics", "[test_diagnostics]") {
    TempDir root;
    flog::LogWriter writer;
    flog::MetricsCapture capture(root.path() / std::string(flog::kMetricsName), writer);
    for (int value = 0; value < 110; ++value) {
        capture.record("stop_to_result_s", value);
        capture.record("secret", 1);
        capture.record("cancel_s", std::numeric_limits<double>::quiet_NaN());
    }
    capture.record("dropped_frames", -1);
    REQUIRE(capture.close(std::chrono::seconds(2)));
    const Json report = flog::collect_report(report_inputs(root.path(), nullptr));
    const Json& metric = report["recent_metrics"]["stop_to_result_s"];
    CHECK(metric["count"] == 100);
    CHECK(metric["latest"] == 109);
    CHECK(metric["maximum"] == 109);
    CHECK(metric["p50"].get<double>() == 59.5);
    CHECK(metric["p95"].get<double>() == 104.05);
    CHECK_FALSE(report["recent_metrics"].contains("cancel_s"));
    CHECK(read_file(capture.path()).find("secret") == std::string::npos);
    CHECK_FALSE(report["recent_metrics"].contains("dropped_frames"));
}

TEST_CASE("the metrics file is json.dumps with default separators, counters as integers",
          "[test_diagnostics]") {
    TempDir root;
    flog::LogWriter writer;
    flog::MetricsCapture capture(root.path() / std::string(flog::kMetricsName), writer);
    capture.record("shutdown_s", 0.25);
    capture.record("captured_frames", 48000);
    capture.record("shutdown_s", 2);
    capture.record("mouse_hook_callback_ms", 1.5);
    REQUIRE(capture.close(std::chrono::seconds(2)));
    CHECK(
        read_file(capture.path()) ==
        R"({"shutdown_s": [0.25, 2.0], "captured_frames": [48000], "mouse_hook_callback_ms": [1.5]})");

    // A new capture starts from the samples on disk.
    flog::MetricsCapture reloaded(capture.path(), writer);
    CHECK(reloaded.samples() == capture.samples());
}

// from tests/test_diagnostics.py::test_record_metrics_is_inert_before_app_logging
TEST_CASE("record metrics is inert before app logging", "[test_diagnostics]") {
    fv::metrics::set_sink({});
    fv::metrics::record("stop_to_result_s", 1);

    // A capture receives the facade's values only while installed.
    TempDir root;
    flog::LogWriter writer;
    flog::MetricsCapture capture(root.path() / std::string(flog::kMetricsName), writer);
    fv::metrics::record("stop_to_result_s", 1);
    capture.install();
    fv::metrics::record("stop_to_result_s", 2);
    capture.uninstall();
    fv::metrics::record("stop_to_result_s", 3);
    CHECK(capture.samples() == Json{{"stop_to_result_s", Json::array({2.0})}});
}

// from
// tests/test_diagnostics.py::test_stalled_metric_storage_does_not_block_qt_or_build_write_queue The
// Qt timer of the Python test is the caller here: the point is that record() returns at once,
// whatever the writer is doing.
TEST_CASE("stalled metric storage does not block qt or build write queue", "[test_diagnostics]") {
    TempDir root;
    flog::LogWriter writer;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    std::vector<Json> snapshots;
    flog::MetricsCapture capture(
        root.path() / std::string(flog::kMetricsName), writer,
        [&](const std::filesystem::path&, const Json& snapshot) {
            std::unique_lock lock(mutex);
            snapshots.push_back(snapshot);
            entered = true;
            changed.notify_all();
            // No Catch2 assertion here: this runs on fv-log. A missed release fails the write.
            if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return release; })) {
                throw std::runtime_error("never released");
            }
        });
    auto release_writer = [&] {
        std::lock_guard lock(mutex);
        release = true;
        changed.notify_all();
    };
    try {
        capture.record("stop_to_result_s", 1);
        {
            std::unique_lock lock(mutex);
            REQUIRE(changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
        }
        auto started = std::chrono::steady_clock::now();
        for (int value = 0; value < 1000; ++value) {
            capture.record("stop_to_result_s", value);
        }
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500));
        CHECK(capture.samples()["stop_to_result_s"].size() == flog::kMaxMetricSamples);
        {
            std::lock_guard lock(mutex);
            CHECK(snapshots.size() == 1);
        }
        started = std::chrono::steady_clock::now();
        CHECK_FALSE(capture.close(std::chrono::milliseconds(20)));
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500));
    } catch (...) {
        release_writer();
        throw;
    }
    release_writer();
    CHECK(capture.close(std::chrono::seconds(2)));
    std::lock_guard lock(mutex);
    REQUIRE(snapshots.size() == 2);
    CHECK(snapshots.back()["stop_to_result_s"].back() == 999);
}

// from tests/test_diagnostics.py::test_logging_close_flushes_shutdown_metric_with_bounded_wait
TEST_CASE("logging close flushes shutdown metric with bounded wait", "[test_diagnostics]") {
    TempDir root;
    flog::LogWriter writer;
    flog::MetricsCapture capture(root.path() / std::string(flog::kMetricsName), writer);
    flog::FailureJournalSink handler(root.path() / std::string(flog::kJournalName), writer);
    capture.record("shutdown_s", .25);
    const auto started = std::chrono::steady_clock::now();
    capture.close(); // what LoggingHandle does at shutdown: at most 100 ms
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500));
    CHECK(capture.flush(std::chrono::seconds(2)));
    CHECK(fv::py::loads(read_file(capture.path()))["shutdown_s"] == Json::array({.25}));
}

// from tests/test_diagnostics.py::test_malformed_settings_and_journal_are_safe
TEST_CASE("malformed settings and journal are safe", "[test_diagnostics]") {
    const std::string content = GENERATE("not JSON", "[]", "null");
    TempDir root;
    write_file(root.path() / std::string(fv::identity().settings_file), content);
    write_file(root.path() / std::string(flog::kJournalName), content);
    const Json settings =
        flog::read_json_file(root.path() / std::string(fv::identity().settings_file));
    const Json report = flog::collect_report(report_inputs(root.path(), &settings));
    CHECK(report["recent_failures"] == Json::array());
    CHECK(report["engine"]["selected_model"] == "default_or_custom");
}

TEST_CASE("read_json_file refuses large, non-UTF-8 and BOM-prefixed files", "[test_diagnostics]") {
    TempDir root;
    const auto path = root.path() / "x.json";
    write_file(path, "[1, NaN]");
    CHECK(flog::read_json_file(path).size() == 2);
    write_file(path, "[\"\xff\"]");
    CHECK(flog::read_json_file(path).is_null());
    write_file(path, "\xEF\xBB\xBF[]");
    CHECK(flog::read_json_file(path).is_null());
    write_file(path, "[" + std::string(1024 * 1024, ' ') + "]");
    CHECK(flog::read_json_file(path).is_null());
    write_file(path, "[" + std::string(1024 * 1024 - 2, ' ') + "]");
    CHECK(flog::read_json_file(path) == Json::array());
    CHECK(flog::read_json_file(root.path() / "missing.json").is_null());
}

TEST_CASE("C++ exceptions map to Python's categories", "[test_diagnostics]") {
    using flog::category_for_exception;
    CHECK(category_for_exception(nullptr) == "LoggedError");
    const std::bad_alloc no_memory;
    CHECK(category_for_exception(&no_memory) == "MemoryError");
    const std::system_error timed_out(std::make_error_code(std::errc::timed_out));
    CHECK(category_for_exception(&timed_out) == "TimeoutError");
    const std::system_error not_permitted(std::make_error_code(std::errc::operation_not_permitted));
    CHECK(category_for_exception(&not_permitted) == "PermissionError");
    const std::system_error in_progress(
        std::make_error_code(std::errc::connection_already_in_progress));
    CHECK(category_for_exception(&in_progress) == "OSError");
    const std::out_of_range range("x");
    CHECK(category_for_exception(&range) == "ValueError");
    const std::domain_error domain("x");
    CHECK(category_for_exception(&domain) == "ValueError");
    const std::overflow_error overflow("x");
    CHECK(category_for_exception(&overflow) == "RuntimeError");
    const std::logic_error logic("x");
    CHECK(category_for_exception(&logic) == "Error");
    // A real failed file operation, whatever the platform's error code.
    try {
        (void)std::filesystem::file_size(std::filesystem::temp_directory_path() /
                                         "fv-log-test-missing" / "missing.bin");
        FAIL("file_size of a missing file succeeded");
    } catch (const std::filesystem::filesystem_error& error) {
        CHECK(category_for_exception(&error) == "FileNotFoundError");
    }
#if defined(_WIN32)
    const std::system_error denied(5 /* ERROR_ACCESS_DENIED */, std::system_category());
    CHECK(category_for_exception(&denied) == "PermissionError");
#endif
}
