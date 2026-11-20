// The log line format, the rotating file, the console sink and the writer thread.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/log.h"
#include "log/console_sink.h"
#include "log/rotating_file_sink.h"
#include "log/writer.h"
#include "log_test_support.h"

using fv::logtest::read_file;
using fv::logtest::read_lines;
using fv::logtest::TempDir;
namespace flog = fv::log;

namespace {

const std::chrono::time_zone* utc() {
    return std::chrono::locate_zone("UTC");
}

std::chrono::system_clock::time_point at_20261114_102030_123456() {
    using namespace std::chrono;
    return sys_days{year{2026} / 11 / 14} + hours(10) + minutes(20) + seconds(30) +
           microseconds(123456);
}

flog::Record record(flog::Level level, std::string_view logger, std::string_view message) {
    flog::Record r{};
    r.level = level;
    r.logger = logger;
    r.message = message;
    r.time = at_20261114_102030_123456();
    return r;
}

// Blocks the writer until released, so a test can stall storage on purpose.
class Gate {
public:
    void enter_and_wait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait_for(lock, std::chrono::seconds(10), [&] { return released_; });
    }
    bool wait_entered() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(5), [&] { return entered_; });
    }
    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool released_ = false;
};

#if defined(_WIN32)
constexpr std::uintmax_t kNewline = 2; // Python's text mode writes "\r\n"
#else
constexpr std::uintmax_t kNewline = 1;
#endif

} // namespace

TEST_CASE("log lines read asctime - LEVEL - name - message, in local time to the millisecond",
          "[log]") {
    CHECK(flog::format_record(record(flog::Level::Info, "fv.test", "message"), utc()) ==
          "2026-11-14 10:20:30,123 - INFO - fv.test - message");
    CHECK(flog::format_record(record(flog::Level::Warning, "root", "x"),
                              std::chrono::locate_zone("Europe/Rome")) ==
          "2026-11-14 11:20:30,123 - WARNING - root - x");
    CHECK(flog::level_name(flog::Level::Debug) == "DEBUG");
    CHECK(flog::level_name(flog::Level::Error) == "ERROR");
    CHECK(flog::level_name(flog::Level::Critical) == "CRITICAL");
}

TEST_CASE("the log file is appended to in UTF-8 with Python's text-mode line ends", "[log]") {
    TempDir folder;
    const auto file = folder.path() / "app.log";
    fv::logtest::write_file(file, "earlier\n");
    {
        flog::LogWriter writer;
        flog::RotatingFileSink sink(file, writer, flog::kLogMaxBytes, flog::kLogBackupCount, utc());
        sink.write(record(flog::Level::Info, "fv.test", "première ligne"));
        sink.write(record(flog::Level::Error, "fv.test", "two\nlines"));
        REQUIRE(writer.flush(std::chrono::seconds(5)));
    }
#if defined(_WIN32)
    CHECK(read_file(file) == "earlier\n"
                             "2026-11-14 10:20:30,123 - INFO - fv.test - première ligne\r\n"
                             "2026-11-14 10:20:30,123 - ERROR - fv.test - two\r\nlines\r\n");
#else
    CHECK(read_file(file) == "earlier\n"
                             "2026-11-14 10:20:30,123 - INFO - fv.test - première ligne\n"
                             "2026-11-14 10:20:30,123 - ERROR - fv.test - two\nlines\n");
#endif
}

TEST_CASE("rotation at 5 MiB keeps .1 to .3", "[log]") {
    CHECK(flog::kLogMaxBytes == 5u * 1024 * 1024);
    CHECK(flog::kLogBackupCount == 3);
    TempDir folder;
    const auto file = folder.path() / "fiorivoce.log";
    const std::string chunk(64 * 1024, 'x');
    {
        flog::LogWriter writer;
        flog::RotatingFileSink sink(file, writer, flog::kLogMaxBytes, flog::kLogBackupCount, utc());
        // About 26 MiB: the file fills five times over, so the oldest rotated file falls off.
        for (int i = 0; i < 400; ++i) {
            const std::string message = std::to_string(i) + " " + chunk;
            sink.write(record(flog::Level::Info, "fv.test", message));
        }
        REQUIRE(writer.flush(std::chrono::seconds(60)));
        CHECK(writer.dropped() == 0);
    }
    for (const char* suffix : {"", ".1", ".2", ".3"}) {
        auto path = file;
        path += suffix;
        INFO(path.string());
        REQUIRE(std::filesystem::exists(path));
        CHECK(std::filesystem::file_size(path) < flog::kLogMaxBytes);
    }
    auto fourth = file;
    fourth += ".4";
    CHECK_FALSE(std::filesystem::exists(fourth));
    // Rolled files are full: one more line would have reached 5 MiB.
    auto first = file;
    first += ".1";
    CHECK(std::filesystem::file_size(first) + 64 * 1024 + 50 >= flog::kLogMaxBytes);
    // The newest line is last in the live file, and the files are in order.
    const auto lines = read_lines(file);
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.back().find(" - 399 x") != std::string::npos);
    auto third = file;
    third += ".3";
    const auto oldest = read_lines(third);
    REQUIRE_FALSE(oldest.empty());
    const auto number_of = [](const std::string& line) {
        const auto at = line.find(" - fv.test - ") + 13;
        return std::stoi(line.substr(at, line.find(' ', at) - at));
    };
    CHECK(number_of(oldest.back()) < number_of(read_lines(first).front()));
}

// RotatingFileHandler.shouldRollover: never on an empty file; otherwise when tell() plus the
// formatted line's length in code points, with its "\n", reaches maxBytes.
TEST_CASE("rollover compares bytes on disk with the line's length in code points", "[log]") {
    // "2026-11-14 10:20:30,123 - INFO - x - " is 37 characters; 13 more make a 50-character line.
    const std::string ascii(13, 'a');
    std::string accented;
    for (int i = 0; i < 13; ++i) {
        accented += "é";
    }
    const auto files_after_three = [&](std::uintmax_t max_bytes, const std::string& message) {
        TempDir folder;
        const auto file = folder.path() / "x.log";
        {
            flog::LogWriter writer;
            flog::RotatingFileSink sink(file, writer, max_bytes, 3, utc());
            for (int i = 0; i < 3; ++i) {
                sink.write(record(flog::Level::Info, "x", message));
            }
            REQUIRE(writer.flush(std::chrono::seconds(5)));
        }
        std::vector<std::size_t> counts;
        for (const char* suffix : {"", ".1", ".2", ".3"}) {
            auto path = file;
            path += suffix;
            counts.push_back(std::filesystem::exists(path) ? read_lines(path).size() : 0);
        }
        return counts;
    };
    // After one line the file holds 50 + newline bytes; the next line counts 51.
    CHECK(files_after_three(50 + kNewline + 51, ascii) == std::vector<std::size_t>{1, 1, 1, 0});
    CHECK(files_after_three(50 + kNewline + 52, ascii) == std::vector<std::size_t>{1, 2, 0, 0});
    // 13 "é" are 26 bytes but 13 code points: the second line still fits.
    CHECK(files_after_three(63 + kNewline + 52, accented) == std::vector<std::size_t>{1, 2, 0, 0});
    // A single line longer than the limit goes into the empty file regardless.
    CHECK(files_after_three(10, ascii) == std::vector<std::size_t>{1, 1, 1, 0});
}

TEST_CASE("the console sink writes the formatted line and a newline on fv-log", "[log]") {
    flog::LogWriter writer;
    std::mutex mutex;
    std::string written;
    flog::ConsoleSink sink(
        writer,
        [&](std::string_view text) {
            std::lock_guard lock(mutex);
            written += text;
        },
        utc());
    sink.write(record(flog::Level::Critical, "qt", "fatal"));
    REQUIRE(writer.flush(std::chrono::seconds(5)));
    std::lock_guard lock(mutex);
    CHECK(written == "2026-11-14 10:20:30,123 - CRITICAL - qt - fatal\n");
}

TEST_CASE("a stalled sink never blocks Logger::info: the queue drops the oldest with a counter",
          "[log]") {
    TempDir folder;
    const auto file = folder.path() / "app.log";
    Gate gate;
    flog::LogWriter writer(8);
    auto sink = std::make_shared<flog::RotatingFileSink>(file, writer, flog::kLogMaxBytes,
                                                         flog::kLogBackupCount, utc());
    writer.post([&] { gate.enter_and_wait(); });
    REQUIRE(gate.wait_entered());

    fv::logtest::CapturedLog captured;
    flog::add_sink(sink);
    constexpr flog::Logger kTest{"fv.test"};
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        kTest.info("line {}", i);
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    flog::remove_sink(sink);
    CHECK(elapsed < std::chrono::milliseconds(500));
    CHECK(writer.dropped() == 92);
    CHECK_FALSE(writer.flush(std::chrono::milliseconds(20)));

    gate.release();
    REQUIRE(writer.flush(std::chrono::seconds(5)));
    const auto lines = read_lines(file);
    REQUIRE(lines.size() == 8);
    CHECK(lines.front().ends_with(" - INFO - fv.test - line 92"));
    CHECK(lines.back().ends_with(" - INFO - fv.test - line 99"));
    CHECK(captured.sink().entries().size() == 100);
}

TEST_CASE("coalesced work runs once per batch of wakes and is never dropped", "[log]") {
    Gate gate;
    flog::LogWriter writer(1);
    std::atomic<int> runs{0};
    auto job = writer.coalesced([&] { ++runs; });
    writer.post([&] { gate.enter_and_wait(); });
    REQUIRE(gate.wait_entered());
    for (int i = 0; i < 50; ++i) {
        job.wake();
        writer.post([] {});
    }
    gate.release();
    REQUIRE(writer.flush(std::chrono::seconds(5)));
    CHECK(runs == 1);
    CHECK(writer.dropped() == 49);
    job.wake();
    REQUIRE(writer.flush(std::chrono::seconds(5)));
    CHECK(runs == 2);
}

TEST_CASE("stop leaves a writer stuck in a job behind instead of waiting for it", "[log]") {
    auto gate = std::make_shared<Gate>();
    std::chrono::steady_clock::time_point started;
    {
        flog::LogWriter writer;
        writer.post([gate] { gate->enter_and_wait(); });
        REQUIRE(gate->wait_entered());
        started = std::chrono::steady_clock::now();
        writer.stop(std::chrono::milliseconds(50));
        writer.post([] {}); // ignored after stop
    }
    // The destructor does not wait again: stop's budget was the whole wait.
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(800));
    gate->release();
}

TEST_CASE("flush waits for the work submitted before it, not for a count of jobs", "[log]") {
    auto gate = std::make_shared<Gate>();
    auto line_ran = std::make_shared<std::atomic<bool>>(false);
    flog::LogWriter writer;
    auto job = writer.coalesced([] {});
    writer.post([gate] { gate->enter_and_wait(); });
    REQUIRE(gate->wait_entered());
    writer.post([line_ran] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        *line_ran = true;
    });
    std::atomic<bool> flushed{false};
    std::atomic<bool> ran_when_flushed{false};
    std::thread flusher([&] {
        flushed = writer.flush(std::chrono::seconds(5));
        ran_when_flushed = line_ran->load();
    });
    // The flush has taken its mark by now; work woken after it runs first (woken jobs go before
    // lines) but must not stand in for the line.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    job.wake();
    gate->release();
    flusher.join();
    CHECK(flushed);
    CHECK(ran_when_flushed);
}

TEST_CASE("a sink stays safe to call after its writer is destroyed", "[log]") {
    TempDir folder;
    auto writer = std::make_unique<flog::LogWriter>();
    auto file = std::make_shared<flog::RotatingFileSink>(
        folder.path() / "app.log", *writer, flog::kLogMaxBytes, flog::kLogBackupCount, utc());
    std::atomic<int> printed{0};
    auto console =
        std::make_shared<flog::ConsoleSink>(*writer, [&](std::string_view) { ++printed; }, utc());
    file->write(record(flog::Level::Info, "fv.test", "before"));
    REQUIRE(writer->flush(std::chrono::seconds(5)));
    // The facade calls sinks from a snapshot, so another thread may still be inside a removed
    // sink after the logging was released.
    writer.reset();
    file->write(record(flog::Level::Info, "fv.test", "after"));
    console->write(record(flog::Level::Info, "fv.test", "after"));
    file->flush(std::chrono::seconds(1));
    console->flush(std::chrono::seconds(1));
    const auto lines = read_lines(folder.path() / "app.log");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].ends_with(" - before"));
    CHECK(printed == 0);
}
