#pragma once

#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "base/hash.h"
#include "base/log.h"
#include "base/testing/manual_executor.h"
#include "speech/service.h"
#include "testkit/testkit.h"

namespace fv::speech::test {

inline std::filesystem::path fixture(const std::string& name) {
    return testkit::source_root() / "tests" / "fixtures" / "audio" / name;
}

// The config models::resolve_parakeet gives for an installed runtime and model.
inline BackendConfig parakeet_config(std::string device = "cuda", std::string requested = "") {
    BackendConfig config;
    config.backend = "parakeet";
    config.runtime_dir = "runtime";
    config.model_path = "model.gguf";
    config.model_key = "parakeet-v3";
    config.model_label = "Parakeet TDT 0.6B v3";
    config.requested_device = requested.empty() ? device : requested;
    config.device = std::move(device);
    config.gpu_index = 0;
    return config;
}

inline std::shared_ptr<const std::vector<float>> shared(std::vector<float> samples) {
    return std::make_shared<const std::vector<float>>(std::move(samples));
}

inline AudioClip clip(std::vector<float> samples, double offset_s = 0.0) {
    AudioClip made;
    made.end = samples.size();
    made.samples = shared(std::move(samples));
    made.offset_s = offset_s;
    return made;
}

inline DecodeJob job(AudioClip input, Lane lane = Lane::Final, std::string language = "en") {
    DecodeJob made;
    made.input = std::move(input);
    made.lane = lane;
    made.options.language = std::move(language);
    return made;
}

// Runs what the speech thread posted to `executor` until `done` holds; false when the time ran
// out first.
inline bool pump_until(ManualExecutor& executor, const std::function<bool()>& done,
                       std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        executor.run_all();
        if (done()) {
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Every record logged while it lives (Python's caplog).
class LogCapture {
public:
    LogCapture() : sink_(std::make_shared<Sink>()) { log::add_sink(sink_); }
    ~LogCapture() { log::remove_sink(sink_); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    // "LEVEL logger: message" lines, joined.
    std::string text() const {
        std::lock_guard lock(sink_->mutex);
        return sink_->text;
    }

private:
    struct Sink final : log::Sink {
        std::mutex mutex;
        std::string text;
        void write(const log::Record& record) override {
            static constexpr const char* kLevels[] = {"DEBUG", "INFO", "WARNING", "ERROR",
                                                      "CRITICAL"};
            std::lock_guard lock(mutex);
            text += std::string(kLevels[static_cast<int>(record.level)]) + " " +
                    std::string(record.logger) + ": " + std::string(record.message) + "\n";
        }
    };
    std::shared_ptr<Sink> sink_;
};

// The SHA-256 of float32 little-endian bytes, as the vectors record windows.
inline std::string digest(std::span<const float> samples) {
    static_assert(std::endian::native == std::endian::little);
    Sha256 hash;
    hash.update(std::as_bytes(samples));
    return hash.hexdigest();
}

// 64-bit LCG; each step's top 16 bits as int16 (tools/parity/cases/services.local_asr.audio.py).
inline std::vector<std::int16_t> lcg_int16(std::size_t count, std::uint64_t seed) {
    std::vector<std::int16_t> out(count);
    std::uint64_t state = seed;
    for (auto& value : out) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        value = static_cast<std::int16_t>(static_cast<std::int64_t>(state >> 48) - 32768);
    }
    return out;
}

// services.local_asr.audio's named signals, as int16.
inline std::vector<std::int16_t> stream_signal(const std::string& kind, std::size_t count,
                                               std::int64_t seed) {
    constexpr std::int64_t block = 1600;
    const auto amp = [seed](std::int64_t index) {
        return (index / block * 7919 + seed) % 64 + 1;
    };
    std::vector<std::int16_t> out(count, 0);
    if (kind == "silence") {
        return out;
    }
    if (kind == "constant") {
        out.assign(count, static_cast<std::int16_t>(seed));
        return out;
    }
    if (kind == "noise" || kind == "gaps") {
        const auto noise = lcg_int16(count, static_cast<std::uint64_t>(seed));
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = static_cast<std::int64_t>(i);
            std::int64_t value = (static_cast<std::int64_t>(noise[i]) * amp(index)) >> 6;
            if (kind == "gaps" && (index / block) % 23 == 17) {
                value = 0;
            }
            out[i] = static_cast<std::int16_t>(value);
        }
        return out;
    }
    if (kind == "sine") {
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = static_cast<std::int64_t>(i);
            const std::int64_t phase = index % 160;
            const std::int64_t q = phase % 80;
            std::int64_t base = (16 * q * (80 - q) * 32000) / (5 * 80 * 80 - 4 * q * (80 - q));
            if (phase >= 80) {
                base = -base;
            }
            out[i] = static_cast<std::int16_t>((base * amp(index)) >> 6);
        }
        return out;
    }
    throw std::invalid_argument("unknown signal kind " + kind);
}

// A 16-bit PCM WAV with the bytes Python's wave module writes: a 44-byte header, then the
// interleaved samples.
inline void write_wav(const std::filesystem::path& path, int rate, int channels,
                      std::span<const std::int16_t> samples) {
    const auto u32 = [](std::ofstream& out, std::uint32_t value) {
        const char bytes[4] = {static_cast<char>(value), static_cast<char>(value >> 8),
                               static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
        out.write(bytes, 4);
    };
    const auto u16 = [](std::ofstream& out, std::uint16_t value) {
        const char bytes[2] = {static_cast<char>(value), static_cast<char>(value >> 8)};
        out.write(bytes, 2);
    };
    const auto data = static_cast<std::uint32_t>(samples.size() * 2);
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4);
    u32(out, 36 + data);
    out.write("WAVEfmt ", 8);
    u32(out, 16);
    u16(out, 1);
    u16(out, static_cast<std::uint16_t>(channels));
    u32(out, static_cast<std::uint32_t>(rate));
    u32(out, static_cast<std::uint32_t>(rate * channels * 2));
    u16(out, static_cast<std::uint16_t>(channels * 2));
    u16(out, 16);
    out.write("data", 4);
    u32(out, data);
    for (const std::int16_t sample : samples) {
        u16(out, static_cast<std::uint16_t>(sample));
    }
}

} // namespace fv::speech::test
