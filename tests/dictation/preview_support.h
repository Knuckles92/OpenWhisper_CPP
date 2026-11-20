#pragma once

// The live preview and early decode around a TranscriptionRuntime: an engine that answers each
// lane with its own text and can hold one lane's decodes, a recorder that captures onto a real
// PcmTape and saves it as a WAV, and a StreamingRuntime whose window preview ticks and delivers
// on a ManualExecutor the test drives. Its signals are recorded on the RuntimeRig's inline UI.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "audio/pcm_tape.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "dictation/recorder_port.h"
#include "dictation/streaming_runtime.h"
#include "dictation/transcription_runtime.h"
#include "runtime_support.h"
#include "speech/incremental_session.h"
#include "speech/service.h"

namespace fv::dictation::testing {

inline constexpr int kRate = 44100; // config.SAMPLE_RATE
inline constexpr int kBlock = 1024; // config.CHUNK_SIZE

using Pcm = std::vector<std::int16_t>;

// int(frames * 16000 / 44100): the samples a preview window of `frames` reaches the engine with.
inline std::size_t preview_samples(std::int64_t frames) {
    return static_cast<std::size_t>(static_cast<double>(frames * 16000) / kRate);
}

// Whole recorder blocks of a 220 Hz tone, `seconds` long (rounded to blocks).
inline Pcm tone(double seconds, double amplitude = 8000.0) {
    const auto blocks = static_cast<std::int64_t>(std::lround(seconds * kRate / kBlock));
    Pcm samples(static_cast<std::size_t>(blocks * kBlock));
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<std::int16_t>(
            std::sin(2.0 * 3.141592653589793 * 220.0 * static_cast<double>(i) / kRate) * amplitude);
    }
    return samples;
}

// A deterministic generator for speech_like.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed * 0x9E3779B97F4A7C15ULL + 1) {}
    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    double uniform(double low = 0.0, double high = 1.0) {
        return low + (high - low) * static_cast<double>(next() >> 11) * 0x1.0p-53;
    }
    double normal() {
        double sum = 0.0;
        for (int i = 0; i < 12; ++i) {
            sum += uniform();
        }
        return sum - 6.0;
    }

private:
    std::uint64_t state_;
};

// Bursts of tones and noise between quiet gaps, so the windows' split points vary
// (test_incremental_dictation.py speech_like).
inline Pcm speech_like(double seconds, std::uint64_t seed) {
    Rng rng(seed);
    const auto total = static_cast<std::size_t>(seconds * kRate);
    Pcm out;
    out.reserve(total + 2 * kRate);
    while (out.size() < total) {
        const auto n = static_cast<std::size_t>(rng.uniform(0.2, 1.6) * kRate);
        if (rng.uniform() < 0.3) {
            for (std::size_t i = 0; i < n; ++i) {
                out.push_back(static_cast<std::int16_t>(std::lround(rng.normal() * 30)));
            }
        } else {
            const double frequency = rng.uniform(120, 900);
            const double amplitude = rng.uniform(1500, 9000);
            for (std::size_t i = 0; i < n; ++i) {
                const double value =
                    std::sin(2 * 3.141592653589793 * frequency * static_cast<double>(i) / kRate) *
                        amplitude +
                    rng.normal() * 800;
                out.push_back(
                    static_cast<std::int16_t>(std::clamp(std::lround(value), -32768L, 32767L)));
            }
        }
    }
    out.resize(total);
    return out;
}

// A 16-bit PCM WAV of `samples` at `rate`.
inline void write_wav(const std::filesystem::path& path, std::span<const std::int16_t> samples,
                      int rate, int channels = 1) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const auto put32 = [&](std::uint32_t value) {
        const char bytes[4] = {static_cast<char>(value & 0xFF), static_cast<char>(value >> 8),
                               static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
        out.write(bytes, 4);
    };
    const auto put16 = [&](std::uint16_t value) {
        const char bytes[2] = {static_cast<char>(value & 0xFF), static_cast<char>(value >> 8)};
        out.write(bytes, 2);
    };
    const auto data = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    out.write("RIFF", 4);
    put32(36 + data);
    out.write("WAVEfmt ", 8);
    put32(16);
    put16(1);
    put16(static_cast<std::uint16_t>(channels));
    put32(static_cast<std::uint32_t>(rate));
    put32(static_cast<std::uint32_t>(rate * channels * 2));
    put16(static_cast<std::uint16_t>(channels * 2));
    put16(16);
    out.write("data", 4);
    put32(data);
    out.write(reinterpret_cast<const char*>(samples.data()), data);
    if (!out) {
        throw std::runtime_error("could not write the test WAV");
    }
}

// One decode the engine took.
struct LaneCall {
    speech::Lane lane = speech::Lane::Final;
    std::size_t samples = 0;
    std::string language;
};

// The engine behind the service: a text per lane, call by call, and a gate for one lane.
// Affinity: thread-safe.
struct LaneEngine {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<LaneCall> calls;
    std::size_t created = 0;
    std::size_t releases = 0;
    // (lane, the lane's call number from 1, samples): the window's text. By default a preview
    // window is "p<n>", a final decode "Hello there." and anything else "".
    std::function<std::string(speech::Lane, std::size_t, std::span<const float>)> text =
        [](speech::Lane lane, std::size_t call, std::span<const float>) -> std::string {
        if (lane == speech::Lane::Preview) {
            return "p" + std::to_string(call);
        }
        return lane == speech::Lane::Final ? "Hello there." : "";
    };
    // Decodes on this lane wait at the gate until open() (or the 10 s safety limit).
    std::optional<speech::Lane> held;
    bool open_gate = false;
    std::size_t waiting = 0;

    std::vector<LaneCall> all() {
        std::lock_guard lock(mutex);
        return calls;
    }
    std::vector<speech::Lane> lanes() {
        std::vector<speech::Lane> out;
        for (const LaneCall& call : all()) {
            out.push_back(call.lane);
        }
        return out;
    }
    std::size_t count(speech::Lane lane) {
        std::size_t n = 0;
        for (const LaneCall& call : all()) {
            n += call.lane == lane ? 1 : 0;
        }
        return n;
    }
    void hold(speech::Lane lane) {
        std::lock_guard lock(mutex);
        held = lane;
        open_gate = false;
    }
    void open() {
        {
            std::lock_guard lock(mutex);
            open_gate = true;
        }
        changed.notify_all();
    }
    bool wait_waiting(std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [this] { return waiting > 0; });
    }
    bool wait_calls(std::size_t count,
                    std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return calls.size() >= count; });
    }

    speech::BackendFactory factory(std::shared_ptr<LaneEngine> self);
};

class LaneBackend final : public speech::Backend {
public:
    explicit LaneBackend(std::shared_ptr<LaneEngine> engine) : engine_(std::move(engine)) {}
    std::string_view id() const override { return "parakeet"; }
    speech::Capabilities capabilities() const override { return {}; }
    speech::Readiness load(const CancelToken&) override {
        return speech::Readiness{speech::ReadyState::Ready, "Parakeet TDT 0.6B v3 | cpu", ""};
    }
    speech::DecodeResult decode(const speech::DecodeJob& job) override {
        const auto& clip = std::get<speech::AudioClip>(job.input);
        const std::span<const float> samples(clip.samples->data() + clip.begin,
                                             clip.end - clip.begin);
        speech::DecodeResult result;
        std::unique_lock lock(engine_->mutex);
        std::size_t lane_call = 1;
        for (const LaneCall& call : engine_->calls) {
            lane_call += call.lane == job.lane ? 1 : 0;
        }
        engine_->calls.push_back(LaneCall{job.lane, samples.size(), job.options.language});
        engine_->changed.notify_all();
        if (engine_->held && *engine_->held == job.lane && !engine_->open_gate) {
            ++engine_->waiting;
            engine_->changed.notify_all();
            engine_->changed.wait_for(lock, std::chrono::seconds(10),
                                      [this] { return engine_->open_gate; });
            --engine_->waiting;
        }
        result.transcript.text = engine_->text(job.lane, lane_call, samples);
        return result;
    }
    void release() override {
        std::lock_guard lock(engine_->mutex);
        ++engine_->releases;
    }

private:
    std::shared_ptr<LaneEngine> engine_;
};

inline speech::BackendFactory LaneEngine::factory(std::shared_ptr<LaneEngine> self) {
    return [self](const speech::BackendConfig&) -> std::unique_ptr<speech::Backend> {
        {
            std::lock_guard lock(self->mutex);
            ++self->created;
        }
        return std::make_unique<LaneBackend>(self);
    };
}

// The text of a decode that depends only on its samples, so the same audio gives the same text
// whichever path cut it (the early windows and the saved file's).
inline std::string content_label(std::span<const float> samples) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const float sample : samples) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &sample, sizeof bits);
        hash = (hash ^ bits) * 1099511628211ULL;
    }
    return "Window " + std::to_string(hash % 100000) + ".";
}

// A recorder whose capture goes onto a PcmTape; a save writes the tape and the recorder's
// 500 ms of silence as the WAV.
// Affinity: thread-safe for what the runtime calls; append() from one thread at a time.
class TapeRecorder final : public RecorderPort {
public:
    TapeRecorder(std::filesystem::path output, Trace& trace)
        : output_(std::move(output)), trace_(trace) {}

    bool is_recording() const override { return recording.load(); }
    bool recorder_busy() const override { return false; }
    bool capture_canceled() const override { return canceled.load(); }
    bool stream_open() const override { return recording.load(); }
    bool start_recording() override {
        trace_.add("recorder_start");
        {
            std::lock_guard lock(mutex_);
            tape_ = std::make_shared<audio::PcmTape>(kRate, channels);
        }
        canceled = false;
        recording = true;
        return true;
    }
    std::string last_start_error() const override { return {}; }
    bool stop_recording() override {
        trace_.add("recorder_stop");
        recording = false;
        return true;
    }
    bool wait_for_stop_completion() override {
        trace_.add("waited");
        if (on_wait) {
            on_wait();
        }
        return true;
    }
    bool has_recording_data() const override {
        const auto tape = current();
        return tape && tape->frames() > 0;
    }
    bool save_recording() override {
        trace_.add("saved");
        if (on_save) {
            on_save();
        }
        if (!save_ok.load()) {
            return false;
        }
        const auto tape = current();
        Pcm samples(static_cast<std::size_t>(tape->frames() * tape->channels()));
        tape->view(0, tape->frames()).copy_to(samples);
        samples.resize(samples.size() + static_cast<std::size_t>(kRate / 2 * tape->channels()), 0);
        write_wav(output_, samples, kRate, tape->channels());
        return true;
    }
    double get_recording_duration() const override {
        const auto tape = current();
        return tape ? static_cast<double>(tape->frames()) / kRate : 0.0;
    }
    std::string last_capture_error() const override {
        std::lock_guard lock(mutex_);
        return capture_error_;
    }
    void fail_capture(const std::string& message) override {
        trace_.add("fail_capture:" + message);
    }
    void cancel_recording() override {
        trace_.add("recorder_cancel");
        canceled = true;
        recording = false;
    }
    void acknowledge_recording() override { trace_.add("acknowledged"); }
    const std::filesystem::path& output_file() const override { return output_; }
    Signal<std::string>& capture_error() override { return capture_error_signal_; }
    std::shared_ptr<const audio::PcmTape> tape() const override { return current(); }

    // The microphone hears `samples` (interleaved, whole frames).
    void append(std::span<const std::int16_t> samples) {
        const auto tape = current();
        tape->append(samples);
    }
    // The capture fails with `message`, as the recorder reports it.
    void report_capture_error(const std::string& message) {
        {
            std::lock_guard lock(mutex_);
            capture_error_ = message;
        }
        capture_error_signal_.emit(message);
    }
    std::shared_ptr<audio::PcmTape> current() const {
        std::lock_guard lock(mutex_);
        return tape_;
    }

    std::atomic<bool> recording{false};
    std::atomic<bool> canceled{false};
    std::atomic<bool> save_ok{true};
    int channels = 1;
    // Set before use.
    std::function<void()> on_wait;
    std::function<void()> on_save;

private:
    std::filesystem::path output_;
    Trace& trace_;
    mutable std::mutex mutex_;
    std::shared_ptr<audio::PcmTape> tape_;
    std::string capture_error_;
    Signal<std::string> capture_error_signal_;
};

struct PreviewRigOptions {
    Json settings = [] {
        Json initial = paste_settings();
        initial["streaming_enabled"] = true;
        initial["streaming_chunk_duration"] = 3.0;
        return initial;
    }();
    bool load_engine = true;
    // Set the preview up (setup()) once the rig is built.
    bool set_up = true;
};

// A TranscriptionRuntime with its live preview, over a LaneEngine and a TapeRecorder.
struct PreviewRig {
    explicit PreviewRig(PreviewRigOptions options = {})
        : engine(std::make_shared<LaneEngine>()), rig([&] {
              RigOptions rig_options;
              rig_options.settings = options.settings;
              rig_options.load_engine = options.load_engine;
              rig_options.factory = engine->factory(engine);
              rig_options.recorder = [this](const std::filesystem::path& root) -> RecorderPort& {
                  recorder =
                      std::make_unique<TapeRecorder>(root / "recorded_audio.wav", recorder_trace);
                  return *recorder;
              };
              return rig_options;
          }()) {
        streaming = std::make_unique<StreamingRuntime>(*rig.speech, *recorder, rig.settings,
                                                       preview_ui, clock);
        links.push_back(
            streaming->partial_transcription.connect(rig.ui, [this](std::string text, bool final) {
                std::lock_guard lock(mutex);
                partials.emplace_back(std::move(text), final);
            }));
        links.push_back(
            streaming->streaming_text_update.connect(rig.ui, [this](std::string text, bool final) {
                std::lock_guard lock(mutex);
                text_updates.emplace_back(std::move(text), final);
            }));
        links.push_back(streaming->streaming_overlay_show.connect(rig.ui, [this] {
            std::lock_guard lock(mutex);
            ++overlay_shows;
        }));
        links.push_back(streaming->streaming_overlay_hide.connect(rig.ui, [this] {
            std::lock_guard lock(mutex);
            ++overlay_hides;
        }));
        links.push_back(streaming->status.connect(rig.ui, [this](std::string text) {
            std::lock_guard lock(mutex);
            statuses.push_back(std::move(text));
        }));
        rig.runtime->set_streaming(streaming.get());
        if (options.set_up) {
            streaming->setup();
        }
    }
    ~PreviewRig() {
        rig.runtime->set_streaming(nullptr);
        links.clear();
        streaming.reset();
    }
    PreviewRig(const PreviewRig&) = delete;
    PreviewRig& operator=(const PreviewRig&) = delete;

    // Runs the preview's poll timer, and what the speech thread posted, until `done` holds.
    bool drive(const std::function<bool()>& done,
               std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            preview_ui.advance(std::chrono::milliseconds(100));
            if (done()) {
                return true;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // Until `count` preview texts reached partial_transcription.
    bool drive_partials(std::size_t count) {
        return drive([&] {
            std::lock_guard lock(mutex);
            return partials.size() >= count;
        });
    }
    // Until `count` preview windows were decoded and none is in flight.
    bool drive_windows(std::size_t count) {
        return drive([&] { return engine->count(speech::Lane::Preview) >= count; });
    }

    std::vector<std::pair<std::string, bool>> partial_texts() {
        std::lock_guard lock(mutex);
        return partials;
    }
    std::vector<std::pair<std::string, bool>> updates() {
        std::lock_guard lock(mutex);
        return text_updates;
    }
    int shows() {
        std::lock_guard lock(mutex);
        return overlay_shows;
    }
    int hides() {
        std::lock_guard lock(mutex);
        return overlay_hides;
    }
    std::vector<std::string> streaming_statuses() {
        std::lock_guard lock(mutex);
        return statuses;
    }

    std::shared_ptr<LaneEngine> engine;
    Trace recorder_trace;
    std::unique_ptr<TapeRecorder> recorder;
    RuntimeRig rig;
    ManualExecutor preview_ui;
    FakeClock clock;
    std::unique_ptr<StreamingRuntime> streaming;
    std::vector<Subscription> links;

    std::mutex mutex;
    std::vector<std::pair<std::string, bool>> partials;
    std::vector<std::pair<std::string, bool>> text_updates;
    int overlay_shows = 0;
    int overlay_hides = 0;
    std::vector<std::string> statuses;
};

} // namespace fv::dictation::testing
