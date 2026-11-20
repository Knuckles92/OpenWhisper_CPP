#pragma once

// Shared pieces of the fv_bench tests: files in a temporary folder, AMI word files, a 16-bit
// PCM WAV writer, and a scripted speech engine behind a real SpeechService.
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "bench/engine.h"
#include "speech/service.h"

namespace fv::bench::test {

inline void write_file(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

// A mono 16-bit PCM WAV.
inline void write_wav(const std::filesystem::path& path, std::span<const std::int16_t> samples,
                      int rate) {
    const auto put32 = [](std::string& out, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        }
    };
    const auto put16 = [](std::string& out, std::uint16_t v) {
        out.push_back(static_cast<char>(v & 0xFF));
        out.push_back(static_cast<char>(v >> 8));
    };
    std::string out = "RIFF";
    const auto data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
    put32(out, 36 + data_bytes);
    out += "WAVEfmt ";
    put32(out, 16);
    put16(out, 1);
    put16(out, 1);
    put32(out, static_cast<std::uint32_t>(rate));
    put32(out, static_cast<std::uint32_t>(rate * 2));
    put16(out, 2);
    put16(out, 16);
    out += "data";
    put32(out, data_bytes);
    for (const std::int16_t sample : samples) {
        put16(out, static_cast<std::uint16_t>(sample));
    }
    write_file(path, out);
}

// A 220 Hz tone at amplitude 8000: audio the preview never mistakes for digital silence.
inline std::vector<std::int16_t> tone(std::size_t frames, int rate) {
    std::vector<std::int16_t> samples(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        samples[i] = static_cast<std::int16_t>(
            std::sin(2.0 * 3.141592653589793 * 220.0 * static_cast<double>(i) / rate) * 8000.0);
    }
    return samples;
}

// The engine behind the service: records each clip's length and answers with `text` (call
// numbers from 1); a `text` that throws fails that decode.
struct ScriptedSpeech {
    std::mutex mutex;
    std::vector<std::size_t> lengths;
    std::function<std::string(std::size_t call)> text = [](std::size_t) {
        return std::string("word");
    };

    std::vector<std::size_t> call_lengths() {
        std::lock_guard lock(mutex);
        return lengths;
    }
};

class ScriptedBackend final : public speech::Backend {
public:
    explicit ScriptedBackend(std::shared_ptr<ScriptedSpeech> speech) : speech_(std::move(speech)) {}
    std::string_view id() const override { return "parakeet"; }
    speech::Capabilities capabilities() const override { return {}; }
    speech::Readiness load(const CancelToken&) override {
        return speech::Readiness{speech::ReadyState::Ready, "Scripted | cpu", ""};
    }
    speech::DecodeResult decode(const speech::DecodeJob& job) override {
        const auto& clip = std::get<speech::AudioClip>(job.input);
        std::function<std::string(std::size_t)> text;
        std::size_t call = 0;
        {
            std::lock_guard lock(speech_->mutex);
            speech_->lengths.push_back(clip.end - clip.begin);
            call = speech_->lengths.size();
            text = speech_->text;
        }
        speech::DecodeResult result;
        result.transcript.text = text(call);
        result.transcript.segments.push_back(Segment{result.transcript.text, 0.0, 1.0});
        return result;
    }
    void release() override {}

private:
    std::shared_ptr<ScriptedSpeech> speech_;
};

inline std::unique_ptr<BenchEngine> scripted_engine(std::shared_ptr<ScriptedSpeech> speech) {
    speech::BackendConfig config = parakeet_config(EngineFiles{"runtime", "model.gguf", "cpu"});
    return std::make_unique<BenchEngine>(
        [speech](const speech::BackendConfig&) {
            return std::make_unique<ScriptedBackend>(speech);
        },
        config);
}

} // namespace fv::bench::test
