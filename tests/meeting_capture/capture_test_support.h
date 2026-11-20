#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "base/json.h"
#include "base/log.h"
#include "base/testing/fake_clock.h"
#include "meeting_capture/capture_block.h"
#include "meeting_capture/spool.h"

namespace fv::meeting::test {

// Every log record of the test, kept as text.
class LogCapture {
public:
    struct Entry {
        log::Level level;
        std::string logger;
        std::string message;
        std::string exception_type;
    };

    LogCapture() : sink_(std::make_shared<Sink>()) {
        log::set_level(log::Level::Debug);
        log::add_sink(sink_);
    }
    ~LogCapture() { log::remove_sink(sink_); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    std::vector<Entry> entries() const {
        const std::lock_guard lock(sink_->mutex);
        return sink_->entries;
    }
    void clear() {
        const std::lock_guard lock(sink_->mutex);
        sink_->entries.clear();
    }
    // Records of `logger` at `level`.
    std::vector<Entry> of(std::string_view logger, log::Level level) const {
        std::vector<Entry> out;
        for (const Entry& entry : entries()) {
            if (entry.logger == logger && entry.level == level) {
                out.push_back(entry);
            }
        }
        return out;
    }
    // Levels of the records whose message contains `text`, in order.
    std::vector<log::Level> levels(std::string_view text) const {
        std::vector<log::Level> out;
        for (const Entry& entry : entries()) {
            if (entry.message.find(text) != std::string::npos) {
                out.push_back(entry.level);
            }
        }
        return out;
    }

private:
    struct Sink final : log::Sink {
        void write(const log::Record& record) override {
            const std::lock_guard lock(mutex);
            entries.push_back({record.level, std::string(record.logger),
                               std::string(record.message), std::string(record.exception_type)});
        }
        mutable std::mutex mutex;
        std::vector<Entry> entries;
    };
    std::shared_ptr<Sink> sink_;
};

// A new folder under the test's own temporary folder, removed afterwards.
class TempDir {
public:
    TempDir() {
        std::random_device seed;
        const auto base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            path_ = base / ("fv-meeting-capture-" + std::to_string(seed()));
            if (std::filesystem::create_directory(path_)) {
                return;
            }
        }
        throw std::runtime_error("no temporary folder");
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (const auto& entry : std::filesystem::directory_iterator(path_)) {
            out.push_back(entry.path().filename().string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

private:
    std::filesystem::path path_;
};

// The golden vectors' signal segments: [kind, frames, amp, step], "saw" being
// ((i * step) % 65536) * 2 * amp // 65536 - amp with i counted from the signal's start.
inline std::vector<std::int16_t> make_signal(const Json& segments) {
    std::vector<std::int16_t> out;
    std::int64_t offset = 0;
    for (const Json& segment : segments) {
        const std::string kind = segment.at(0).get<std::string>();
        const std::int64_t frames = segment.at(1).get<std::int64_t>();
        const std::int64_t amp = segment.at(2).get<std::int64_t>();
        const std::int64_t step = segment.at(3).get<std::int64_t>();
        for (std::int64_t i = offset; i < offset + frames; ++i) {
            std::int64_t value = 0;
            if (kind == "saw") {
                // Every operand is non-negative, so C++'s division floors as Python's does.
                value = ((i * step) % 65536) * 2 * amp / 65536 - amp;
            } else if (kind == "const") {
                value = amp;
            } else if (kind != "zero") {
                throw std::runtime_error("unknown segment " + kind);
            }
            out.push_back(static_cast<std::int16_t>(value));
        }
        offset += frames;
    }
    return out;
}

// OpenWhisper's test _signal(): a 440 Hz int16 tone with silent stretches every quiet_every_s.
inline std::vector<std::int16_t> tone(double duration_s, int rate, double quiet_every_s = 0.0,
                                      double quiet_len_s = 0.6, double amp = 6000.0) {
    const auto n = static_cast<std::size_t>(duration_s * rate);
    std::vector<std::int16_t> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::int16_t>(
            amp * std::sin(2.0 * 3.141592653589793 * 440.0 * static_cast<double>(i) / rate));
    }
    if (quiet_every_s > 0.0) {
        const auto step = static_cast<std::size_t>(quiet_every_s * rate);
        const auto span = static_cast<std::size_t>(quiet_len_s * rate);
        for (std::size_t start = step; start < n; start += step) {
            for (std::size_t i = start; i < std::min(n, start + span); ++i) {
                out[i] = 0;
            }
        }
    }
    return out;
}

// Feeds a block, waiting for room first, so a test that hands blocks faster than real time
// never drops one (OpenWhisper's tests give the queue 100000 slots instead).
inline void feed_waiting(SpoolWriter& writer, const CaptureBlock& block) {
    while (writer.queued_blocks() + 2 > writer.queue_capacity()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    writer.feed(block);
}

inline CaptureBlock block_of(const std::vector<std::int16_t>& samples, std::size_t at,
                             std::size_t count, int rate, double t_mono) {
    return CaptureBlock{.channel = "mic",
                        .frames = samples.data() + at,
                        .count = count,
                        .rate = rate,
                        .t_mono = t_mono};
}

// A FakeClock whose steady time reads `seconds`.
inline void set_steady(FakeClock& clock, double seconds) {
    const double now = std::chrono::duration<double>(clock.steady().time_since_epoch()).count();
    clock.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(seconds - now)));
}

} // namespace fv::meeting::test
