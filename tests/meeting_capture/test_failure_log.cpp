// Ported from OpenWhisper tests/test_meeting_failure_logging.py: failures on the meeting path are
// visible once, quiet on repeats, and private. The engine handlers of the last four tests (the
// diarization chunk reload, the term-corrections read, the TypeSafe judge) arrive with the
// engine; here they run as the same FailureLog keys and calls those handlers make.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "audio/testing/fake_backend.h"
#include "base/testing/fake_clock.h"
#include "capture_test_support.h"
#include "meeting_capture/capture_session.h"
#include "meeting_capture/detail/wav_file.h"
#include "meeting_capture/failure_log.h"
#include "meeting_capture/testing/fake_capture_source.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "pyport/errors.h"

using fv::log::Level;
using fv::meeting::FailureLog;
using fv::meeting::test::LogCapture;
using fv::meeting::test::TempDir;

namespace {

constexpr fv::log::Logger test_logger("test.failure_log");

// Throws in a named function, so the type-only text has a frame to name.
void handle_confidential_failure(FailureLog& log) {
    try {
        throw fv::py::ValueError("what the host said in confidence");
    } catch (const std::exception&) {
        log.failed("key", "Observe failed", true);
    }
}

class NullSink final : public fv::meeting::CaptureStatusSink {
public:
    fv::Json capture_state() override { return nullptr; } // no store
    bool update_capture(const fv::Json&) override { return false; }
    void emit_status(std::string) override {}
    void emit_error(std::string, std::string) override {}
};

} // namespace

// from
// tests/test_meeting_failure_logging.py::TestFailureLog::test_first_failure_is_an_error_with_traceback_and_repeats_are_debug
TEST_CASE("TestFailureLog first failure is an error with traceback and repeats are debug",
          "[test_meeting_failure_logging]") {
    const LogCapture logs;
    FailureLog log(test_logger);
    for (int i = 0; i < 3; ++i) {
        try {
            throw std::system_error(std::make_error_code(std::errc::no_such_device), "disk gone");
        } catch (const std::system_error&) {
            log.failed("key", "Write failed for chunk");
        }
    }
    const auto errors = logs.of("test.failure_log", Level::Error);
    const auto debugs = logs.of("test.failure_log", Level::Debug);
    REQUIRE(errors.size() == 1);
    CHECK(debugs.size() == 2);
    CHECK(errors[0].message.starts_with("Write failed for chunk"));
    CHECK(errors[0].message.ends_with("; repeats are logged at debug until it recovers"));
    // The record carries the exception, as exc_info does.
    CHECK(errors[0].exception_type == "std::system_error");
}

// from
// tests/test_meeting_failure_logging.py::TestFailureLog::test_recovery_lets_the_next_failure_be_reported_again
TEST_CASE("TestFailureLog recovery lets the next failure be reported again",
          "[test_meeting_failure_logging]") {
    const LogCapture logs;
    FailureLog log(test_logger);
    for (bool recover : {false, true, false}) {
        if (recover) {
            log.recovered("key");
        }
        try {
            throw std::runtime_error("x");
        } catch (const std::runtime_error&) {
            log.failed("key", "Probe failed");
        }
    }
    CHECK(logs.of("test.failure_log", Level::Error).size() == 2);
}

// from
// tests/test_meeting_failure_logging.py::TestFailureLog::test_type_only_says_where_it_failed_and_drops_the_message
TEST_CASE("TestFailureLog type only says where it failed and drops the message",
          "[test_meeting_failure_logging]") {
    const LogCapture logs;
    FailureLog log(test_logger);
    handle_confidential_failure(log);
    const auto entries = logs.entries();
    REQUIRE(entries.size() == 1);
    const auto& record = entries.front();
    CHECK(record.level == Level::Error);
    // The type and the place it was handled, never what the error said.
    CHECK(record.message.find("fv::py::ValueError at test_failure_log.cpp:") != std::string::npos);
    CHECK(record.message.find("in handle_confidential_failure") != std::string::npos);
    CHECK(record.message.find("confidence") == std::string::npos);
    CHECK(record.exception_type.empty());
}

// from
// tests/test_meeting_failure_logging.py::TestWatchdogRateLimit::test_a_lasting_probe_failure_warns_once_per_channel
TEST_CASE("TestWatchdogRateLimit a lasting probe failure warns once per channel",
          "[test_meeting_failure_logging]") {
    TempDir folder;
    fv::FakeClock clock;
    fv::meeting::MeetingClock meeting_clock(clock);
    auto repository = std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>(clock);
    auto backend = std::make_shared<fv::audio::testing::FakeBackend>();
    backend->set_layout({{.name = "MME", .default_input_device = 0}},
                        {{.name = "Mic", .hostapi = 0, .max_input_channels = 1}}, 0);
    backend->set_loopbacks({{.id = "{speakers}", .name = "Speakers", .is_default = true}});
    NullSink sink;
    // Three passes, then recording() ends the loop.
    std::atomic<int> passes{0};
    fv::meeting::MeetingCapture capture(
        {}, meeting_clock, repository, backend, sink, [&] { return ++passes <= 3; },
        [](const fv::meeting_state::SpooledChunk&) {}, nullptr);
    std::vector<std::shared_ptr<fv::meeting::testing::FakeCaptureSource>> made;
    fv::meeting::SourceFactory factory;
    factory.mic = [&](const fv::audio::InputDevice& device) {
        auto source =
            std::make_shared<fv::meeting::testing::FakeCaptureSource>("mic", device.index);
        // The device enumeration fails from the first watchdog pass on.
        source->throw_from_is_active_after(1, "device enumeration failed");
        made.push_back(source);
        return source;
    };
    factory.loopback = [&](const fv::audio::LoopbackEndpoint& endpoint) {
        auto source =
            std::make_shared<fv::meeting::testing::FakeCaptureSource>("loopback", endpoint.id);
        source->throw_from_is_active_after(1, "device enumeration failed");
        made.push_back(source);
        return source;
    };
    capture.set_source_factory(factory);
    capture.set_timing({.watchdog_interval_s = 0.0, .retry_interval_s = 3.0});
    const LogCapture logs;
    REQUIRE(capture.start("m_test", folder.path()) == std::nullopt);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (passes.load() < 4 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    capture.stop();

    std::vector<LogCapture::Entry> errors;
    std::vector<LogCapture::Entry> debugs;
    for (const auto& entry : logs.of("meeting.capture_session", Level::Error)) {
        if (entry.message.find("Capture watchdog failed") != std::string::npos) {
            errors.push_back(entry);
        }
    }
    for (const auto& entry : logs.of("meeting.capture_session", Level::Debug)) {
        if (entry.message.find("Capture watchdog failed") != std::string::npos) {
            debugs.push_back(entry);
        }
    }
    CHECK(errors.size() == 2); // one per channel, not one per pass
    for (const auto& entry : errors) {
        CHECK(entry.exception_type == "std::runtime_error");
    }
    CHECK(debugs.size() == 4);
}

// from
// tests/test_meeting_failure_logging.py::TestDiarizationChunkReload::test_unreadable_chunks_are_skipped_and_the_rest_still_labeled
TEST_CASE("TestDiarizationChunkReload unreadable chunks are skipped and the rest still labeled",
          "[test_meeting_failure_logging]") {
    TempDir folder;
    const auto write_wav = [&](const std::string& name, int sample_width) {
        const auto path = folder.path() / name;
        std::ofstream out(path, std::ios::binary);
        out << fv::meeting::detail::wav_header(16000, 1, sample_width,
                                               static_cast<std::uint32_t>(sample_width * 160));
        out << std::string(static_cast<std::size_t>(sample_width * 160), '\0');
        return path;
    };
    const auto garbage = folder.path() / "garbage.wav";
    std::ofstream(garbage, std::ios::binary) << "not a wav file at all";
    const std::vector<std::filesystem::path> paths = {
        folder.path() / "missing.wav", // OSError
        garbage,                       // wave.Error
        write_wav("eight_bit.wav", 1), // ValueError
        write_wav("good.wav", 2),
    };
    const LogCapture logs;
    FailureLog failures(fv::log::Logger("meeting.engine"));
    std::vector<std::pair<int, int>> assigned;
    int chunk_id = 0;
    for (const auto& path : paths) {
        ++chunk_id;
        // The engine's handler takes only what reading a WAV raises (OSError, EOFError,
        // wave.Error, ValueError in Python).
        const auto reload_failed = [&] {
            failures.failed("reload_chunk", "Failed to reload chunk " + std::to_string(chunk_id) +
                                                " for diarization");
        };
        try {
            const auto [frames, rate] = fv::meeting::detail::load_wav_int16(path);
            failures.recovered("reload_chunk");
            assigned.emplace_back(chunk_id, rate);
        } catch (const std::system_error&) {
            reload_failed();
        } catch (const fv::meeting::detail::WaveError&) {
            reload_failed();
        } catch (const fv::meeting::detail::WaveEofError&) {
            reload_failed();
        } catch (const fv::py::ValueError&) {
            reload_failed();
        }
    }
    CHECK(assigned == std::vector<std::pair<int, int>>{{4, 16000}});
    CHECK(logs.levels("Failed to reload chunk") ==
          std::vector{Level::Error, Level::Debug, Level::Debug});
}

// from
// tests/test_meeting_failure_logging.py::TestTypeOnlyLogging::test_term_corrections_read_failure_names_only_the_type
TEST_CASE("TestTypeOnlyLogging term corrections read failure names only the type",
          "[test_meeting_failure_logging]") {
    const LogCapture logs;
    FailureLog failures(fv::log::Logger("meeting.engine"));
    for (int i = 0; i < 2; ++i) {
        try {
            throw fv::py::ValueError("rename Project Falcon to Osprey");
        } catch (const std::exception&) {
            failures.failed("term_rules",
                            "Could not read meeting term corrections; decoding without them", true);
        }
    }
    const auto entries = logs.entries();
    std::vector<Level> levels;
    for (const auto& entry : entries) {
        if (entry.message.find("term corrections") == std::string::npos) {
            continue;
        }
        levels.push_back(entry.level);
        CHECK(entry.message.find("ValueError") != std::string::npos);
        CHECK(entry.message.find("Falcon") == std::string::npos);
        CHECK(entry.exception_type.empty());
    }
    CHECK(levels == std::vector{Level::Error, Level::Debug});
}

// from
// tests/test_meeting_failure_logging.py::TestTypeOnlyLogging::test_judge_setup_failure_hides_the_key_and_warns_once
TEST_CASE("TestTypeOnlyLogging judge setup failure hides the key and warns once",
          "[test_meeting_failure_logging]") {
    const LogCapture logs;
    FailureLog failures(fv::log::Logger("meeting.engine"));
    for (int i = 0; i < 2; ++i) {
        try {
            throw fv::py::ValueError("invalid key sk-live-123");
        } catch (const std::exception&) {
            failures.failed("typesafe_judge", "TypeSafe judge unavailable", true);
        }
    }
    std::vector<Level> levels;
    for (const auto& entry : logs.entries()) {
        if (entry.message.find("TypeSafe judge") == std::string::npos) {
            continue;
        }
        levels.push_back(entry.level);
        CHECK(entry.message.find("sk-live") == std::string::npos);
        CHECK(entry.exception_type.empty());
    }
    CHECK(levels == std::vector{Level::Error, Level::Debug});
}

TEST_CASE("where_it_failed without an exception", "[test_meeting_failure_logging]") {
    CHECK(fv::meeting::where_it_failed(static_cast<const std::exception*>(nullptr)) ==
          "unknown error");
}
