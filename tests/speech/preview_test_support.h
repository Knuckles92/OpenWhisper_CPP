#pragma once

// The window preview's harness: a scripted engine behind a real SpeechService, the preview on a
// ManualExecutor, and recorder-sized blocks on a PcmTape.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "audio/pcm_tape.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "speech/service.h"
#include "speech/window_preview.h"
#include "speech_test_support.h"

namespace fv::speech::test {

inline constexpr int kRate = 44100;          // config.SAMPLE_RATE
inline constexpr int kBlock = 1024;          // config.CHUNK_SIZE
inline constexpr int kWindowBlocks = 130;    // blocks that reach 3.0 s at 44.1 kHz
inline constexpr int kOverlapFrames = 33075; // int(0.75 * 44100)

using Block = std::vector<std::int16_t>;

// int(frames * 16000 / 44100): the samples a window of `frames` reaches the engine with.
inline std::size_t resampled(std::int64_t frames) {
    return static_cast<std::size_t>(static_cast<double>(frames * 16000) / kRate);
}

// Recorder-sized int16 noise blocks, distinct per seed (_blocks).
inline std::vector<Block> noise_blocks(double seconds, std::uint64_t seed) {
    const auto count = static_cast<std::size_t>(std::lround(seconds * kRate / kBlock));
    std::vector<Block> blocks;
    for (std::size_t i = 0; i < count; ++i) {
        Block block = lcg_int16(kBlock, seed * 1000003 + i);
        for (auto& sample : block) {
            sample = static_cast<std::int16_t>(sample / 8);
        }
        blocks.push_back(std::move(block));
    }
    return blocks;
}

// A 220 Hz tone at amplitude 8000 (_tone).
inline Block tone(std::int64_t frames) {
    Block samples(static_cast<std::size_t>(frames));
    for (std::int64_t i = 0; i < frames; ++i) {
        samples[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(
            std::sin(2.0 * 3.141592653589793 * 220.0 * static_cast<double>(i) / kRate) * 8000.0);
    }
    return samples;
}

inline std::vector<Block> tone_blocks(int count) {
    const Block all = tone(static_cast<std::int64_t>(count) * kBlock);
    std::vector<Block> blocks;
    for (int i = 0; i < count; ++i) {
        blocks.emplace_back(all.begin() + i * kBlock, all.begin() + (i + 1) * kBlock);
    }
    return blocks;
}

inline std::vector<Block> slice(const std::vector<Block>& blocks, std::size_t first,
                                std::size_t last = static_cast<std::size_t>(-1)) {
    last = std::min(last, blocks.size());
    return {blocks.begin() + static_cast<std::ptrdiff_t>(first),
            blocks.begin() + static_cast<std::ptrdiff_t>(last)};
}

// The engine behind the service: records every window, returns text made by `text`, and can
// hold one call at a gate.
struct ScriptedEngine {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::size_t> lengths;
    std::vector<std::string> languages;
    std::vector<Lane> lanes;
    std::function<std::string(std::size_t call, std::span<const float>)> text =
        [](std::size_t, std::span<const float>) {
            return std::string("hello there");
        };
    std::function<void()> on_decode;
    std::size_t stall_on = 0;
    bool entered = false;
    bool released = false;

    std::size_t calls() {
        std::lock_guard lock(mutex);
        return lengths.size();
    }
    std::vector<std::size_t> call_lengths() {
        std::lock_guard lock(mutex);
        return lengths;
    }
    bool wait_entered(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [this] { return entered; });
    }
    void release() {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        changed.notify_all();
    }
};

class ScriptedBackend final : public Backend {
public:
    explicit ScriptedBackend(std::shared_ptr<ScriptedEngine> engine) : engine_(std::move(engine)) {}
    std::string_view id() const override { return "parakeet"; }
    Capabilities capabilities() const override { return {}; }
    Readiness load(const CancelToken&) override {
        return Readiness{ReadyState::Ready, "Fake | cpu", ""};
    }
    DecodeResult decode(const DecodeJob& job) override {
        const auto& clip = std::get<AudioClip>(job.input);
        const std::span<const float> samples(clip.samples->data() + clip.begin,
                                             clip.end - clip.begin);
        std::function<void()> hook;
        DecodeResult result;
        {
            std::unique_lock lock(engine_->mutex);
            engine_->lengths.push_back(samples.size());
            engine_->languages.push_back(job.options.language);
            engine_->lanes.push_back(job.lane);
            const std::size_t call = engine_->lengths.size();
            if (call == engine_->stall_on) {
                engine_->entered = true;
                engine_->changed.notify_all();
                engine_->changed.wait_for(lock, std::chrono::seconds(5),
                                          [this] { return engine_->released; });
            }
            result.transcript.text = engine_->text(call, samples);
            hook = engine_->on_decode;
        }
        if (hook) {
            hook();
        }
        return result;
    }
    void release() override {}

private:
    std::shared_ptr<ScriptedEngine> engine_;
};

// The text of _Decoder in test_preview_lazy_tail.py: it depends on the window's content, so any
// change in window boundaries or overlap tails changes the preview.
inline std::string content_text(std::size_t, std::span<const float> audio) {
    double sum = 0.0;
    for (std::size_t i = 0; i < std::min<std::size_t>(4000, audio.size()); ++i) {
        sum += audio[i];
    }
    const auto checksum = std::llround(std::fabs(sum) * 1e4) % 100003;
    return "n" + std::to_string(audio.size()) + " c" + std::to_string(checksum);
}

struct PreviewHarness {
    explicit PreviewHarness(WindowPreviewOptions options = {}, bool load = true)
        : engine(std::make_shared<ScriptedEngine>()),
          service(
              [e = engine](const BackendConfig&) { return std::make_unique<ScriptedBackend>(e); },
              notify),
          preview(service, ui, clock, options) {
        if (load) {
            service.select(Slot::Dictation, parakeet_config("cpu"));
            REQUIRE(pump_until(notify, [this] {
                return service.readiness(Slot::Dictation).state == ReadyState::Ready;
            }));
        }
    }

    // A new recording on a fresh tape.
    void start(std::string job = "job-1") {
        tape = std::make_shared<audio::PcmTape>(kRate);
        preview.start(tape, std::move(job), [this](std::string text, bool final) {
            updates.emplace_back(std::move(text), final);
        });
    }

    void feed(const std::vector<Block>& blocks) {
        for (const Block& block : blocks) {
            tape->append(block);
        }
    }
    void feed(const Block& samples) { tape->append(samples); }

    // Runs the preview's poll timer and what the speech thread posted until `done` holds.
    bool drive(const std::function<bool()>& done,
               std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            ui.advance(std::chrono::milliseconds(100));
            notify.run_all();
            if (done()) {
                return true;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Until `calls` windows were decoded and none is in flight, then their updates delivered:
    // the last window's update is posted to the UI before its decode is marked done, so it may
    // still be queued when the condition first holds.
    bool settle(std::size_t calls) {
        if (!drive([&] { return engine->calls() == calls && !preview.decoding(); })) {
            return false;
        }
        ui.run_all();
        return true;
    }

    std::shared_ptr<ScriptedEngine> engine;
    ManualExecutor notify;
    ManualExecutor ui;
    FakeClock clock;
    SpeechService service;
    WindowPreview preview;
    std::shared_ptr<audio::PcmTape> tape;
    std::vector<std::pair<std::string, bool>> updates;
};

// What the old draining stop returned (_old_drain): every block through the worker's windowing,
// a window per 130 blocks then the remainder, none dropped; on an engine of its own.
inline std::string old_drain(const std::vector<Block>& blocks) {
    PreviewHarness reference;
    reference.engine->text = content_text;
    reference.start("reference");
    reference.feed(blocks);
    reference.preview.stop();
    return reference.preview.finalize().value_or("<discarded>");
}

// The live updates: one per 130-block window decoded before stop (_windows_published).
inline std::vector<std::pair<std::string, bool>> windows_published(const std::vector<Block>& blocks,
                                                                   std::size_t count) {
    std::vector<std::pair<std::string, bool>> out;
    for (std::size_t n = 1; n <= count; ++n) {
        out.emplace_back(old_drain(slice(blocks, 0, kWindowBlocks * n)), true);
    }
    return out;
}

} // namespace fv::speech::test
