#pragma once

// What the recorder tests share: a backend that plays the microphones (the port of OpenWhisper's
// FakeSd/StreamFactory/FakeStream in tests/test_personalize_s7_recorder.py over
// audio::testing::FakeBackend), log and metrics capture, an executor that runs signals inline,
// and synthetic blocks.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "audio/backend.h"
#include "audio/testing/fake_backend.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/metrics.h"
#include "base/paths.h"
#include "recorder/recorder.h"
#include "testkit/testkit.h"

namespace fv::recorder_test {

inline std::filesystem::path fixture(const std::string& name) {
    return testkit::source_root() / "tests" / "fixtures" / "audio" / name;
}

// threading.Event for the tests.
class Flag {
public:
    void set() {
        {
            const std::lock_guard lock(mutex_);
            set_ = true;
        }
        changed_.notify_all();
    }
    bool is_set() const {
        const std::lock_guard lock(mutex_);
        return set_;
    }
    bool wait(std::chrono::milliseconds timeout) const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return set_; });
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    bool set_ = false;
};

// Runs every posted task at once on the posting thread, so a Signal reaches its subscriber
// synchronously, as OpenWhisper's callbacks do.
class InlineExecutor final : public Executor {
public:
    void post(Task task) override { task(); }
    TimerHandle post_after(std::chrono::milliseconds, Task) override {
        throw std::logic_error("InlineExecutor has no timers");
    }
    bool is_current() const override { return true; }
};

inline bool wait_until(const std::function<bool()>& predicate,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return predicate();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// The records one logger wrote while the capture lived.
class LogCapture {
public:
    explicit LogCapture(std::string logger = "services.recorder")
        : sink_(std::make_shared<Sink>(std::move(logger))) {
        log::add_sink(sink_);
    }
    ~LogCapture() { log::remove_sink(sink_); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    std::vector<std::string> messages() const {
        const std::lock_guard lock(sink_->mutex);
        return sink_->messages;
    }
    std::vector<std::string> starting_with(std::string_view prefix) const {
        std::vector<std::string> found;
        for (const std::string& message : messages()) {
            if (message.starts_with(prefix)) {
                found.push_back(message);
            }
        }
        return found;
    }

private:
    struct Sink final : log::Sink {
        explicit Sink(std::string name) : logger(std::move(name)) {}
        void write(const log::Record& record) override {
            if (record.logger == logger) {
                const std::lock_guard lock(mutex);
                messages.emplace_back(record.message);
            }
        }
        std::string logger;
        mutable std::mutex mutex;
        std::vector<std::string> messages;
    };
    std::shared_ptr<Sink> sink_;
};

// diagnostics.record_metrics as the tests see it: every value recorded, last one wins.
class MetricsCapture {
public:
    MetricsCapture() {
        metrics::set_sink([this](std::string_view name, double value) {
            const std::lock_guard lock(mutex_);
            values_.emplace_back(std::string(name), value);
        });
    }
    ~MetricsCapture() { metrics::set_sink({}); }
    MetricsCapture(const MetricsCapture&) = delete;
    MetricsCapture& operator=(const MetricsCapture&) = delete;

    std::optional<double> last(std::string_view name) const {
        const std::lock_guard lock(mutex_);
        for (auto it = values_.rbegin(); it != values_.rend(); ++it) {
            if (it->first == name) {
                return it->second;
            }
        }
        return std::nullopt;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, double>> values_;
};

// One capture stream the backend opened, and what the test may do to it besides delivering
// blocks: make it read inactive (unplugged without a finished callback), or make its abort()
// hang as a driver does after the device is removed. Blocks and the finished callback pass
// through it, so a closed stream never reaches the recorder, as PortAudio's close waits for a
// running callback and allows no later one.
struct StreamControl {
    std::atomic<bool> inactive{false};
    std::atomic<bool> hang{false};
    // The driver fails when asked whether the stream is still running.
    std::atomic<bool> broken{false};
    Flag abort_entered;
    Flag release;

    std::mutex mutex; // held while a block or the finished callback is forwarded
    bool closed = false;
    audio::CaptureFn fn = nullptr;
    void* user = nullptr;
    std::function<void(std::string)> on_lost;

    bool is_closed() {
        const std::lock_guard lock(mutex);
        return closed;
    }

    static void forward(void* control, const std::int16_t* samples, std::size_t frames,
                        audio::BlockStatus status) {
        auto* self = static_cast<StreamControl*>(control);
        const std::lock_guard lock(self->mutex);
        if (!self->closed) {
            self->fn(self->user, samples, frames, status);
        }
    }
};

class HookedStream final : public audio::Stream {
public:
    HookedStream(std::unique_ptr<audio::Stream> inner, std::shared_ptr<StreamControl> control)
        : inner_(std::move(inner)), control_(std::move(control)) {}
    ~HookedStream() override {
        {
            const std::lock_guard lock(control_->mutex);
            control_->closed = true;
        }
        inner_.reset();
    }
    HookedStream(const HookedStream&) = delete;
    HookedStream& operator=(const HookedStream&) = delete;
    bool active() const override {
        if (control_->broken.load()) {
            throw std::runtime_error("the driver is gone");
        }
        return !control_->inactive.load() && inner_->active();
    }
    void abort() override {
        if (control_->hang.load()) {
            control_->abort_entered.set();
            control_->release.wait(std::chrono::seconds(10));
        }
        inner_->abort();
    }

private:
    std::unique_ptr<audio::Stream> inner_;
    std::shared_ptr<StreamControl> control_;
};

struct OpenedStream {
    int device = -1; // the index asked for; -1 is PortAudio's default
    std::shared_ptr<audio::testing::FakeStream> fake;
    std::shared_ptr<StreamControl> control;
};

// audio::Backend over a FakeBackend, with OpenWhisper's StreamFactory hooks: indexes that refuse
// to open, a hook before each open, one after each start, and the streams that opened.
class HookedBackend final : public audio::Backend {
public:
    explicit HookedBackend(std::shared_ptr<audio::testing::FakeBackend> fake)
        : fake_(std::move(fake)) {}

    audio::testing::FakeBackend& fake() { return *fake_; }

    void refuse(std::set<int> indexes) {
        const std::lock_guard lock(mutex_);
        refused_ = std::move(indexes);
    }
    void before_open(std::function<void(int)> hook) {
        const std::lock_guard lock(mutex_);
        before_open_ = std::move(hook);
    }
    void on_start(std::function<void(audio::testing::FakeStream&)> hook) {
        const std::lock_guard lock(mutex_);
        on_start_ = std::move(hook);
    }
    std::vector<OpenedStream> opened() const {
        const std::lock_guard lock(mutex_);
        return opened_;
    }
    std::vector<int> opened_devices() const {
        std::vector<int> devices;
        for (const OpenedStream& stream : opened()) {
            devices.push_back(stream.device);
        }
        return devices;
    }
    OpenedStream stream(std::size_t index) const {
        const std::lock_guard lock(mutex_);
        return opened_.at(index);
    }
    std::vector<std::thread::id> listing_threads() const {
        const std::lock_guard lock(mutex_);
        return listing_threads_;
    }

    std::vector<audio::DeviceInfo> inputs() override {
        {
            const std::lock_guard lock(mutex_);
            listing_threads_.push_back(std::this_thread::get_id());
        }
        return fake_->inputs();
    }
    std::vector<audio::DeviceInfo> outputs() override { return fake_->outputs(); }
    std::vector<audio::LoopbackEndpoint> loopbacks() override { return fake_->loopbacks(); }
    std::expected<void, std::string> check_input(const audio::StreamConfig& config) override {
        return fake_->check_input(config);
    }
    std::expected<std::unique_ptr<audio::Stream>, std::string>
    open_capture(const audio::StreamConfig& config, audio::CaptureFn fn, void* user,
                 std::function<void(std::string)> on_lost) override {
        std::function<void(int)> before;
        {
            const std::lock_guard lock(mutex_);
            before = before_open_;
        }
        if (before) {
            before(config.device_index);
        }
        std::function<void(audio::testing::FakeStream&)> started;
        {
            const std::lock_guard lock(mutex_);
            if (refused_.contains(config.device_index)) {
                return std::unexpected(std::string(
                    "Error opening InputStream: Device unavailable [PaErrorCode -9985]"));
            }
            started = on_start_;
        }
        auto control = std::make_shared<StreamControl>();
        control->fn = fn;
        control->user = user;
        control->on_lost = std::move(on_lost);
        StreamControl* raw = control.get();
        auto opened = fake_->open_capture(config, &StreamControl::forward, raw,
                                          [raw](const std::string& reason) {
                                              const std::lock_guard lock(raw->mutex);
                                              if (!raw->closed && raw->on_lost) {
                                                  raw->on_lost(reason);
                                              }
                                          });
        if (!opened) {
            return std::unexpected(opened.error());
        }
        auto fake_stream = fake_->streams().back();
        {
            const std::lock_guard lock(mutex_);
            opened_.push_back(OpenedStream{config.device_index, fake_stream, control});
        }
        if (started) {
            started(*fake_stream);
        }
        return std::unique_ptr<audio::Stream>(
            std::make_unique<HookedStream>(std::move(*opened), std::move(control)));
    }
    std::expected<std::unique_ptr<audio::Stream>, std::string>
    open_loopback(const audio::LoopbackEndpoint& endpoint, audio::CaptureFn fn, void* user,
                  std::function<void(std::string)> on_lost) override {
        return fake_->open_loopback(endpoint, fn, user, std::move(on_lost));
    }
    std::expected<std::unique_ptr<audio::Stream>, std::string>
    open_playback(const audio::StreamConfig& config, audio::PlaybackSource& source) override {
        return fake_->open_playback(config, source);
    }
    bool refresh() override { return fake_->refresh(); }

private:
    std::shared_ptr<audio::testing::FakeBackend> fake_;
    mutable std::mutex mutex_;
    std::set<int> refused_;
    std::function<void(int)> before_open_;
    std::function<void(audio::testing::FakeStream&)> on_start_;
    std::vector<OpenedStream> opened_;
    std::vector<std::thread::id> listing_threads_;
};

inline std::shared_ptr<audio::testing::FakeBackend> windows_fake() {
    return audio::testing::FakeBackend::from_fixture(fixture("devices-windows.json"));
}

// tests/test_personalize_s7_audio_devices.py windows_sd(): the measured Windows layout.
// Devices: 0 Sound Mapper, 1 Blue Snowball (the default), 2 USB (MME), 5 USB (WASAPI), ...
inline const audio::DeviceKey kUsb{"Microphone (2- USB Audio Device", "MME"};
inline const audio::DeviceKey kSnowball{"Microphone (Blue Snowball )", "MME"};

// A recorder over a hooked fake in a temporary data root; everything is torn down in the right
// order (the recorder before the backend it uses).
struct Rig {
    explicit Rig(std::shared_ptr<audio::testing::FakeBackend> fake = windows_fake())
        : backend(std::move(fake)), paths(Paths::resolve(identity(), system_env())) {}

    Recorder& make(RecorderOptions options = {}) {
        recorder = std::make_unique<Recorder>(backend, paths, std::move(options));
        return *recorder;
    }
    Recorder& rec() { return *recorder; }
    std::filesystem::path file(const std::string& name) const { return root.path() / name; }

    testkit::TempDataRoot root;
    HookedBackend backend;
    Paths paths;
    InlineExecutor inline_executor;
    std::unique_ptr<Recorder> recorder;
};

inline std::vector<std::int16_t> tone(std::int16_t value, std::size_t frames = RECORDER_CHUNK) {
    return std::vector<std::int16_t>(frames, value);
}

inline std::vector<std::int16_t> arange(std::size_t count) {
    std::vector<std::int16_t> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = static_cast<std::int16_t>(i);
    }
    return values;
}

// One 1024-frame block of Gaussian noise at `level_db` dBFS RMS (test_recorder_postroll.py
// _block; the generator differs from numpy's, the levels the gate sees do not).
inline std::vector<std::int16_t> noise_block(double level_db, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    std::normal_distribution<double> normal;
    const double rms = std::pow(10.0, level_db / 20.0) * 32768.0;
    std::vector<std::int16_t> block(RECORDER_CHUNK);
    for (std::int16_t& sample : block) {
        const double value = std::nearbyint(normal(random) * rms);
        sample = static_cast<std::int16_t>(std::clamp(value, -32768.0, 32767.0));
    }
    return block;
}

inline std::vector<std::byte> bytes_of(const std::vector<std::int16_t>& samples) {
    const auto view = std::as_bytes(std::span(samples));
    return {view.begin(), view.end()};
}

inline std::string string_of(const std::vector<std::int16_t>& samples) {
    const auto view = std::as_bytes(std::span(samples));
    return {reinterpret_cast<const char*>(view.data()), view.size()};
}

// The data chunk of a WAV this package wrote, as samples.
inline std::vector<std::int16_t> wav_samples(const std::filesystem::path& file) {
    return audio::testing::read_pcm(file);
}

} // namespace fv::recorder_test
