#pragma once

// The fixtures of OpenWhisper's TranscriptionRuntime tests (tests/test_application_controller.py
// _create_controller, tests/test_paste_latency.py _harness, tests/test_cancel_after_stop.py
// _runtime): a recorder the test scripts, the real speech service on a fake engine, History on a
// temporary database, a clipboard and keys that record what they are asked, an event sink, and
// one ordered trace of what the runtime did. The UI executor runs everything at once on the
// posting thread, as Python's tests call the Qt slots directly; the jobs and persistence
// executors queue until the test runs them, as Python's FakeExecutor does.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "appcore/activity.h"
#include "base/events.h"
#include "base/log.h"
#include "base/metrics.h"
#include "base/testing/manual_executor.h"
#include "db/database.h"
#include "dictation/recorder_port.h"
#include "dictation/transcription_runtime.h"
#include "dictation_support.h"
#include "focus/capture_service.h"
#include "focus/testing/fake_clipboard.h"
#include "focus/testing/fake_synthetic_keys.h"
#include "history/dictation_stats.h"
#include "history/manager.h"
#include "history/record_sync.h"
#include "settings/testing/memory_store.h"
#include "speech/service.h"
#include "speech/testing/fake_backend.h"
#include "sql/db.h"
#include "testkit/testkit.h"

namespace fv::dictation::testing {

inline std::filesystem::path audio_fixture(const std::string& name) {
    return testkit::source_root() / "tests" / "fixtures" / "audio" / name;
}

// What happened, in order, across the fakes and the runtime's signals.
// Affinity: thread-safe.
class Trace {
public:
    void add(std::string entry) {
        std::lock_guard lock(mutex_);
        entries_.push_back(std::move(entry));
    }
    std::vector<std::string> entries() const {
        std::lock_guard lock(mutex_);
        return entries_;
    }
    // Index of the first `entry`, or -1.
    std::ptrdiff_t index(const std::string& entry) const {
        const auto all = entries();
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (all[i] == entry) {
                return static_cast<std::ptrdiff_t>(i);
            }
        }
        return -1;
    }
    bool has(const std::string& entry) const { return index(entry) >= 0; }
    std::size_t count(const std::string& entry) const {
        std::size_t n = 0;
        for (const auto& item : entries()) {
            n += item == entry ? 1 : 0;
        }
        return n;
    }
    // The entries starting with `prefix`, the prefix removed.
    std::vector<std::string> with_prefix(const std::string& prefix) const {
        std::vector<std::string> found;
        for (const auto& item : entries()) {
            if (item.starts_with(prefix)) {
                found.push_back(item.substr(prefix.size()));
            }
        }
        return found;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> entries_;
};

// The recorder as OpenWhisper's FakeRecorder and Mock recorders script it. A save copies
// `source` (a real WAV the speech service can decode) to the output file.
// Affinity: thread-safe for what the runtime calls; set the knobs before use.
class FakeRecorder final : public RecorderPort {
public:
    FakeRecorder(std::filesystem::path output, Trace& trace,
                 std::filesystem::path source = audio_fixture("tone-16k-mono.wav"))
        : output_(std::move(output)), source_(std::move(source)), trace_(trace) {}

    bool is_recording() const override { return recording.load(); }
    bool recorder_busy() const override { return false; }
    bool capture_canceled() const override { return canceled.load(); }
    bool stream_open() const override { return recording.load(); }

    bool start_recording() override {
        trace_.add("recorder_start");
        recording = start_ok.load();
        canceled = false;
        {
            std::lock_guard lock(mutex_);
            capture_error_.clear();
        }
        return start_ok.load();
    }
    std::string last_start_error() const override {
        std::lock_guard lock(mutex_);
        return start_error;
    }
    bool stop_recording() override {
        trace_.add("recorder_stop");
        recording = false;
        return stop_ok.load();
    }
    bool wait_for_stop_completion() override {
        trace_.add("waited");
        if (on_wait) {
            on_wait();
        }
        return wait_ok.load();
    }
    bool has_recording_data() const override { return has_data.load(); }
    bool save_recording() override {
        trace_.add("saved");
        if (on_save) {
            on_save();
        }
        if (!save_ok.load()) {
            return false;
        }
        std::filesystem::copy_file(source_, output_,
                                   std::filesystem::copy_options::overwrite_existing);
        return true;
    }
    double get_recording_duration() const override { return duration.load(); }
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
    void acknowledge_recording() override {
        trace_.add("acknowledged");
        ++acknowledged;
    }
    const std::filesystem::path& output_file() const override { return output_; }
    Signal<std::string>& capture_error() override { return capture_error_signal_; }

    // The capture fails with `message`, as the recorder reports it.
    void report_capture_error(const std::string& message) {
        {
            std::lock_guard lock(mutex_);
            capture_error_ = message;
        }
        capture_error_signal_.emit(message);
    }
    void set_capture_error(const std::string& message) {
        std::lock_guard lock(mutex_);
        capture_error_ = message;
    }

    std::atomic<bool> recording{false};
    std::atomic<bool> canceled{false};
    std::atomic<bool> start_ok{true};
    std::atomic<bool> stop_ok{true};
    std::atomic<bool> wait_ok{true};
    std::atomic<bool> has_data{true};
    std::atomic<bool> save_ok{true};
    std::atomic<double> duration{4.0};
    std::atomic<int> acknowledged{0};
    std::string start_error = "No audio device available";
    // Set before use.
    std::function<void()> on_wait;
    std::function<void()> on_save;

private:
    std::filesystem::path output_;
    std::filesystem::path source_;
    Trace& trace_;
    mutable std::mutex mutex_;
    std::string capture_error_;
    Signal<std::string> capture_error_signal_;
};

// A FakeClipboard that also writes each call into the trace, and can fail a stage outright.
// Affinity: thread-safe.
class TracingClipboard final : public focus::ClipboardPort {
public:
    explicit TracingClipboard(Trace& trace, std::string user_text = "user's own clipboard")
        : inner(std::move(user_text)), trace_(trace) {}

    focus::StageResult stage_text(std::string text, std::optional<std::string> html,
                                  bool restore = true) override {
        trace_.add("stage");
        if (stage_error) {
            throw std::runtime_error(*stage_error);
        }
        auto result = inner.stage_text(std::move(text), std::move(html), restore);
        if (restore_unavailable && result.written) {
            result.lease.reset();
            result.restore_unavailable = true;
        }
        return result;
    }
    bool schedule_restore(const focus::ClipboardLease& lease,
                          std::chrono::milliseconds delay) override {
        trace_.add("schedule_restore");
        return inner.schedule_restore(lease, delay);
    }
    bool commit_text(const focus::ClipboardLease& lease, std::string text) override {
        trace_.add("commit");
        return inner.commit_text(lease, std::move(text));
    }
    focus::RestoreOutcome restore_now(const focus::ClipboardLease& lease) override {
        return inner.restore_now(lease);
    }
    void cleanup() override { inner.cleanup(); }
    void request_prefetch(std::chrono::milliseconds delay) override {
        trace_.add("prefetch");
        inner.request_prefetch(delay);
    }
    void discard_prefetch() override {
        trace_.add("discard_prefetch");
        inner.discard_prefetch();
    }
    void capture_selection(std::function<void()> send_copy,
                           std::function<void(std::optional<std::string>)> callback,
                           std::chrono::milliseconds timeout) override {
        inner.capture_selection(std::move(send_copy), std::move(callback), timeout);
    }
    bool write_text(std::string text, std::string html = {}) override {
        trace_.add("copy");
        return inner.write_text(std::move(text), std::move(html));
    }
    bool write_files(focus::ClipboardFiles files) override {
        return inner.write_files(std::move(files));
    }

    focus::FakeClipboard inner;
    std::optional<std::string> stage_error;
    // The stage writes the text but could not keep the user's clipboard.
    bool restore_unavailable = false;

private:
    Trace& trace_;
};

// History's outbox to a paired host, which hears every dictation saved: "history" in the trace.
// Affinity: thread-safe.
class TracingSync final : public RecordSync {
public:
    explicit TracingSync(Trace& trace) : trace_(trace) {}
    void record_saved(std::string_view, std::string_view) override { trace_.add("history"); }
    void record_edited(std::string_view, std::string_view) override {}
    void record_deleted(std::string_view, std::string_view) override {}
    void cleared(std::string_view) override {}

private:
    Trace& trace_;
};

// The events the runtime published.
// Affinity: thread-safe.
class RecordingEvents final : public EventSink {
public:
    void publish(std::string_view type, Json data, bool replay = true) override {
        std::lock_guard lock(mutex_);
        published_.push_back({std::string(type), std::move(data), replay});
    }
    struct Event {
        std::string type;
        Json data;
        bool replay = true;
    };
    std::vector<Event> all() const {
        std::lock_guard lock(mutex_);
        return published_;
    }
    std::vector<Event> of(std::string_view type) const {
        std::vector<Event> found;
        for (auto& event : all()) {
            if (event.type == type) {
                found.push_back(event);
            }
        }
        return found;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Event> published_;
};

// The records one logger wrote while the capture lived (Python's caplog).
class RuntimeLog {
public:
    explicit RuntimeLog(std::string logger = "services.runtime.transcription")
        : sink_(std::make_shared<Sink>(std::move(logger))) {
        log::add_sink(sink_);
    }
    ~RuntimeLog() { log::remove_sink(sink_); }
    RuntimeLog(const RuntimeLog&) = delete;
    RuntimeLog& operator=(const RuntimeLog&) = delete;

    bool contains(std::string_view text) const {
        std::lock_guard lock(sink_->mutex);
        for (const auto& message : sink_->messages) {
            if (message.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    struct Sink final : log::Sink {
        explicit Sink(std::string name) : logger(std::move(name)) {}
        void write(const log::Record& record) override {
            if (record.logger == logger) {
                std::lock_guard lock(mutex);
                messages.emplace_back(record.message);
            }
        }
        std::string logger;
        std::mutex mutex;
        std::vector<std::string> messages;
    };
    std::shared_ptr<Sink> sink_;
};

// diagnostics.record_metrics as the tests see it.
class MetricsLog {
public:
    MetricsLog() {
        metrics::set_sink([this](std::string_view name, double value) {
            std::lock_guard lock(mutex_);
            values_.emplace_back(std::string(name), value);
        });
    }
    ~MetricsLog() { metrics::set_sink({}); }
    MetricsLog(const MetricsLog&) = delete;
    MetricsLog& operator=(const MetricsLog&) = delete;
    std::optional<double> last(std::string_view name) const {
        std::lock_guard lock(mutex_);
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

inline speech::BackendConfig fake_parakeet_config() {
    speech::BackendConfig config;
    config.backend = "parakeet";
    config.runtime_dir = "runtime";
    config.model_path = "model.gguf";
    config.model_key = "parakeet-v3";
    config.model_label = "Parakeet TDT 0.6B v3";
    config.requested_device = "cpu";
    config.device = "cpu";
    return config;
}

// The settings _harness starts from: auto-paste, copy and restore on.
inline Json paste_settings() {
    return Json{{"auto_paste", true}, {"copy_clipboard", true}, {"restore_clipboard", true}};
}

struct RigOptions {
    Json settings = paste_settings();
    speech::FakeScript script = [] {
        speech::FakeScript heard;
        heard.texts = {"Hello there."};
        return heard;
    }();
    // Load the engine before the runtime starts (a ready engine).
    bool load_engine = true;
    // Another engine than the fake's (the real Parakeet backend), and the config to load.
    speech::BackendFactory factory;
    speech::BackendConfig config = fake_parakeet_config();
    // Another recorder than the FakeRecorder, made in the data root.
    std::function<RecorderPort&(const std::filesystem::path& root)> recorder;
    // Adds to the hooks the rig sets before the runtime is made.
    std::function<void(RuntimeHooks&)> hooks;
    // Changes the services the rig set (another capture service, the captures root), last
    // before the runtime is made.
    std::function<void(TranscriptionServices&)> services;
};

// A TranscriptionRuntime over the fakes, in a temporary data root.
struct RuntimeRig {
    explicit RuntimeRig(RigOptions options = {})
        : settings(options.settings), recorder(root.path() / "recorded_audio.wav", trace),
          engine(std::make_shared<speech::FakeEngine>(options.script)),
          speech(std::make_unique<speech::SpeechService>(
              options.factory ? options.factory : engine->factory(), notify)),
          database(std::make_unique<Database>(Database::open_or_create(root.path() / "test.db"))),
          sync(trace),
          history(std::make_unique<HistoryManager>(*database, root.path() / "recordings",
                                                   RecordingRetention{}, history_services())),
          stats(std::make_unique<dictation_stats::StatsRepository>(*database)), clipboard(trace) {
        if (options.load_engine) {
            load(options.config);
        }
        keys.on_send = [this](const std::string& what) {
            trace.add(what);
        };
        TranscriptionServices services;
        services.settings = &settings;
        services.recorder = options.recorder ? &options.recorder(root.path()) : &recorder;
        services.speech = speech.get();
        services.history = history.get();
        services.stats = stats.get();
        services.delivery.clipboard = &clipboard;
        services.delivery.keys = &keys;
        services.activity = &activity;
        services.ui = &ui;
        services.jobs = &jobs;
        services.persistence = &persistence;
        services.events = &events;
        services.capture = &capture;
        services.hooks.stop_playback = [this] {
            trace.add("stop_playback");
        };
        services.hooks.request_reload = [this] {
            trace.add("request_reload");
        };
        services.hooks.request_model_consent = [this] {
            trace.add("request_model_consent");
        };
        services.hooks.readiness_message = [this]() -> std::optional<std::string> {
            std::lock_guard lock(readiness_mutex);
            return readiness;
        };
        if (options.hooks) {
            options.hooks(services.hooks);
        }
        if (options.services) {
            options.services(services);
        }
        runtime = std::make_unique<TranscriptionRuntime>(std::move(services));
        links.push_back(runtime->status_update.connect(
            ui, [this](std::string status) { trace.add("status:" + status); }));
        links.push_back(
            runtime->overlay_state_update.connect(ui, [this](appcore::OverlayState state) {
                trace.add("overlay:" + std::string(appcore::to_string(state)));
            }));
        links.push_back(runtime->notice_requested.connect(ui, [this](std::string text, bool done) {
            trace.add("notice:" + text + (done ? " (done)" : ""));
        }));
        links.push_back(runtime->recording_state_changed().connect(
            ui, [this](bool on) { trace.add(on ? "recording:on" : "recording:off"); }));
        links.push_back(runtime->refresh_history.connect(ui, [this] {
            trace.add("refresh_history");
            if (refresh_error) {
                throw std::runtime_error(*refresh_error);
            }
        }));
        links.push_back(runtime->transcription_completed.connect(
            ui,
            [this](std::string text, std::optional<std::string> raw, std::optional<CleanupInfo>) {
                transcript_text = text;
                transcript_raw = raw;
                trace.add("transcript:" + text);
            }));
        links.push_back(runtime->transcription_failed.connect(
            ui, [this](std::string status) { trace.add("error_dialog:" + status); }));
        links.push_back(runtime->transcription_stats.connect(
            ui, [this](TranscriptionStats value) { stats_shown = value; }));
        links.push_back(runtime->clear_stats.connect(ui, [this] { trace.add("clear_stats"); }));
    }
    ~RuntimeRig() {
        links.clear();
        runtime.reset();
        speech.reset();
        stats.reset();
        history.reset();
        database->close();
    }
    RuntimeRig(const RuntimeRig&) = delete;
    RuntimeRig& operator=(const RuntimeRig&) = delete;

    // Selects the engine and waits until it reports a final readiness.
    void load(speech::BackendConfig config = fake_parakeet_config()) {
        speech->select(speech::Slot::Dictation, std::move(config));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        while (speech->readiness(speech::Slot::Dictation).state == speech::ReadyState::Loading) {
            if (std::chrono::steady_clock::now() > deadline) {
                throw std::runtime_error("the fake engine did not load");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    HistoryServices history_services() {
        HistoryServices services;
        services.record_sync = &sync;
        return services;
    }

    // History's table is gone: every save throws, as a broken database does.
    void break_history() {
        database->write([](sql::Connection& connection) {
            connection.exec("DROP TABLE transcription_history");
        });
    }

    // Runs the jobs and history saves queued, and what they queue, until none is left.
    void run_workers() {
        while (jobs.run_all() + persistence.run_all() > 0) {
        }
    }

    std::vector<std::string> statuses() const { return trace.with_prefix("status:"); }
    std::string last_status() const {
        const auto all = statuses();
        return all.empty() ? std::string() : all.back();
    }
    std::vector<std::string> notices() const { return trace.with_prefix("notice:"); }
    std::vector<HistoryEntry> entries() { return history->get_history(); }
    void set_readiness(std::optional<std::string> message) {
        std::lock_guard lock(readiness_mutex);
        readiness = std::move(message);
    }
    // A WAV the size of the fixture, at `name` in the data root.
    std::filesystem::path wav(const std::string& name) {
        const auto path = root.path() / name;
        std::filesystem::copy_file(audio_fixture("tone-16k-mono.wav"), path,
                                   std::filesystem::copy_options::overwrite_existing);
        return path;
    }

    testkit::TempDataRoot root;
    Trace trace;
    MemorySettingsStore settings;
    FakeRecorder recorder;
    InlineExecutor notify;
    std::shared_ptr<speech::FakeEngine> engine;
    std::unique_ptr<speech::SpeechService> speech;
    std::unique_ptr<Database> database;
    TracingSync sync;
    std::unique_ptr<HistoryManager> history;
    std::unique_ptr<dictation_stats::StatsRepository> stats;
    TracingClipboard clipboard;
    focus::FakeSyntheticKeys keys;
    focus::NullCaptureService capture;
    ActivityState activity;
    InlineExecutor ui;
    ManualExecutor jobs;
    ManualExecutor persistence;
    RecordingEvents events;
    std::mutex readiness_mutex;
    std::optional<std::string> readiness;
    std::optional<std::string> refresh_error;
    std::string transcript_text;
    std::optional<std::string> transcript_raw;
    std::optional<TranscriptionStats> stats_shown;
    std::unique_ptr<TranscriptionRuntime> runtime;
    std::vector<Subscription> links;
};

} // namespace fv::dictation::testing
