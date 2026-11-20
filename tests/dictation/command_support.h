#pragma once

// The fixtures of OpenWhisper's Command Mode tests (tests/test_personalize_s3_command_runtime.py
// h and bare_commands, tests/test_personalize_s3_command_delivery.py command_harness): the real
// TranscriptionRuntime and CommandRuntime over a recorder the test scripts, the fake speech
// engine, History on a temporary database, a clipboard whose selection copies answer as the test
// says, a focus capture service the test swaps, and the real AI cleanup client talking to a
// model that answers in memory. The test's thread is the GUI thread: work posted to the UI from
// it runs at once, work posted from another thread waits for settle() or drain(), as Qt's
// processEvents in the Python tests. Selection reads run on a real thread pool.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/executor.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "cleanup_support.h"
#include "dictation/command_runtime.h"
#include "dictation/transcription_runtime.h"
#include "focus/testing/fake_clipboard.h"
#include "focus/testing/fake_platform.h"
#include "focus/testing/fake_synthetic_keys.h"
#include "llm/rewrite.h"
#include "runtime_support.h"

namespace fv::dictation::testing {

// The apps the Python tests name.
inline focus::AppIdentity notepad_app() {
    return app("notepad.exe", "Notepad", 4, "0x4");
}
inline focus::AppIdentity terminal_app() {
    return app("WindowsTerminal.exe", "Windows Terminal", 5, "0x5");
}
inline focus::AppIdentity vs_code_app() {
    return app("Code.exe", "Visual Studio Code", 6, "0x6");
}
inline focus::AppIdentity browser_app() {
    return app("chrome.exe", "Chrome", 9, "0x9");
}
inline focus::TextContext selected(std::string text) {
    focus::TextContext context;
    context.selected = std::move(text);
    context.selection_known = true;
    return context;
}

// The GUI thread: the thread that made it. A task posted on that thread runs at once; one posted
// from another thread waits for pump(). Timers are kept and never fire.
// Affinity: thread-safe.
class GuiExecutor final : public Executor {
public:
    void post(Task task) override {
        if (is_current()) {
            task();
            return;
        }
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(task));
    }
    TimerHandle post_after(std::chrono::milliseconds, Task) override {
        return TimerHandle(std::make_shared<TimerState>());
    }
    bool is_current() const override { return std::this_thread::get_id() == owner_; }

    // Runs what other threads posted, and what that posts, on the GUI thread.
    std::size_t pump() {
        std::size_t count = 0;
        for (;;) {
            Task task;
            {
                std::lock_guard lock(mutex_);
                if (queue_.empty()) {
                    return count;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
            ++count;
        }
    }

private:
    const std::thread::id owner_ = std::this_thread::get_id();
    std::mutex mutex_;
    std::deque<Task> queue_;
};

// A clipboard that records stages and copies into the trace and answers selection copies with
// `selection` (nullopt: it can't see other apps' copies; `answers` false: never calls back).
// Affinity: thread-safe.
class CommandClipboard final : public focus::ClipboardPort {
public:
    CommandClipboard(Trace& trace, const GuiExecutor& gui) : trace_(trace), gui_(gui) {}

    focus::StageResult stage_text(std::string text, std::optional<std::string> html,
                                  bool restore = true) override {
        trace_.add("stage:" + text);
        return inner.stage_text(std::move(text), std::move(html), restore);
    }
    bool schedule_restore(const focus::ClipboardLease& lease,
                          std::chrono::milliseconds delay) override {
        return inner.schedule_restore(lease, delay);
    }
    bool commit_text(const focus::ClipboardLease& lease, std::string text) override {
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
    void discard_prefetch() override { inner.discard_prefetch(); }
    void capture_selection(std::function<void()> send_copy,
                           std::function<void(std::optional<std::string>)> callback,
                           std::chrono::milliseconds timeout) override {
        std::optional<std::string> answer;
        bool answers_now = true;
        {
            std::lock_guard lock(mutex_);
            captures_.push_back(gui_.is_current());
            timeouts_.push_back(timeout);
            answer = selection;
            answers_now = answers;
        }
        if (answer && send_copy) {
            send_copy();
        }
        if (answers_now) {
            callback(std::move(answer));
        }
    }
    bool write_text(std::string text, std::string html = {}) override {
        trace_.add("copy:" + text);
        return inner.write_text(std::move(text), std::move(html));
    }
    bool write_files(focus::ClipboardFiles files) override {
        return inner.write_files(std::move(files));
    }

    // One entry per selection copy: whether it ran on the GUI thread.
    std::vector<bool> captures() const {
        std::lock_guard lock(mutex_);
        return captures_;
    }
    std::vector<std::chrono::milliseconds> timeouts() const {
        std::lock_guard lock(mutex_);
        return timeouts_;
    }
    void set_selection(std::optional<std::string> text) {
        std::lock_guard lock(mutex_);
        selection = std::move(text);
    }
    void never_answer() {
        std::lock_guard lock(mutex_);
        answers = false;
    }

    focus::FakeClipboard inner;

private:
    Trace& trace_;
    const GuiExecutor& gui_;
    mutable std::mutex mutex_;
    std::optional<std::string> selection = std::string();
    bool answers = true;
    std::vector<bool> captures_;
    std::vector<std::chrono::milliseconds> timeouts_;
};

// Synthetic keys that record what is sent into the trace and where each modifier wait ran.
// Affinity: thread-safe.
class CommandSyntheticKeys final : public focus::SyntheticKeys {
public:
    CommandSyntheticKeys(Trace& trace, const GuiExecutor& gui) : trace_(trace), gui_(gui) {}

    void send_paste() override {
        if (fail_paste.load()) {
            throw focus::SyntheticKeysError("no input desktop");
        }
        trace_.add("paste");
    }
    void send_copy() override { trace_.add("send_copy"); }
    bool modifiers_held() override { return !released.load(); }
    bool wait_for_modifiers_released(std::chrono::milliseconds, const CancelToken&) override {
        std::unique_lock lock(mutex_);
        waits_on_gui_.push_back(gui_.is_current());
        // Held while the test keeps the reader here (up to 5 s).
        gate_.wait_for(lock, std::chrono::seconds(5), [this] { return !held_; });
        return released.load();
    }
    // Keeps every modifier wait from returning until open().
    void hold() {
        std::lock_guard lock(mutex_);
        held_ = true;
    }
    void open() {
        {
            std::lock_guard lock(mutex_);
            held_ = false;
        }
        gate_.notify_all();
    }
    // One entry per modifier wait: whether it ran on the GUI thread.
    std::vector<bool> waits() const {
        std::lock_guard lock(mutex_);
        return waits_on_gui_;
    }

    std::atomic<bool> released{true};
    std::atomic<bool> fail_paste{false};

private:
    Trace& trace_;
    const GuiExecutor& gui_;
    mutable std::mutex mutex_;
    std::condition_variable gate_;
    bool held_ = false;
    std::vector<bool> waits_on_gui_;
};

// The capture service installed now (focus_context.set_service), for the job captures, the
// paste check and Command Mode's own probes.
// Affinity: thread-safe.
class SwitchCapture final : public focus::CaptureService {
public:
    SwitchCapture()
        : current_(std::make_shared<focus::FakeCaptureService>(focus::FocusSnapshot{})) {}

    void set(std::shared_ptr<focus::CaptureService> service) {
        std::lock_guard lock(mutex_);
        current_ = std::move(service);
    }
    // A FakeCaptureService answering `snapshot`, with `current` as the focused app (the
    // snapshot's own when unset).
    std::shared_ptr<focus::FakeCaptureService>
    fake(focus::FocusSnapshot snapshot,
         std::optional<std::optional<focus::AppIdentity>> current = std::nullopt) {
        auto service = std::make_shared<focus::FakeCaptureService>(std::move(snapshot), current);
        set(service);
        return service;
    }

    std::shared_ptr<focus::FocusRequest> request(bool include_text,
                                                 bool include_selection) override {
        return get()->request(include_text, include_selection);
    }
    std::optional<focus::AppIdentity> current_identity() override {
        return get()->current_identity();
    }
    void reread(const std::optional<focus::AppIdentity>& identity,
                std::function<void(std::optional<focus::TextContext>)> callback) override {
        get()->reread(identity, std::move(callback));
    }
    void shutdown() override {}

private:
    std::shared_ptr<focus::CaptureService> get() const {
        std::lock_guard lock(mutex_);
        return current_;
    }
    mutable std::mutex mutex_;
    std::shared_ptr<focus::CaptureService> current_;
};

// Keys from an in-memory store and a map environment: an OpenRouter key, or none at all. No
// .env, and nothing here reads the owner's keys.
struct TestCredentials {
    explicit TestCredentials(bool with_key) {
        if (with_key) {
            env.set("OPENROUTER_API_KEY", "test-openrouter-key");
        }
    }
    std::shared_ptr<MemoryCredentialBackend> backend = std::make_shared<MemoryCredentialBackend>();
    CredentialStore store{backend, identity()};
    MapEnvReader env;
    DotEnv dotenv{std::filesystem::path("fv-dictation-tests-no-such-dir") / ".env"};
    llm::KeySources sources{store, env, dotenv};
};

// The AI model, in memory: every chat request is answered with `reply`, or (stall) held until
// the client gives up, after moving `clock` past any rewrite's deadline. Records every request.
// Affinity: thread-safe.
class ModelClient final : public http::Client {
public:
    explicit ModelClient(FakeClock& clock) : clock_(clock) {}

    std::expected<http::Response, http::Error>
    send(const http::Request& request, const CancelToken& cancel, http::BodySink* sink) override {
        bool stall = false;
        std::string reply;
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(request);
            stall = stall_;
            reply = reply_;
        }
        if (stall) {
            // Past the 60 s budget of the longest rewrite: the client's wait gives up.
            clock_.advance(std::chrono::seconds(61));
            const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!cancel.cancelled() && std::chrono::steady_clock::now() < give_up) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return std::unexpected(http::Error{http::ErrorKind::Cancelled, "canceled"});
        }
        http::FakeHttpClient answer;
        answer.set_handler(chat_answer(reply));
        return answer.send(request, cancel, sink);
    }

    void answer(std::string reply) {
        std::lock_guard lock(mutex_);
        reply_ = std::move(reply);
        stall_ = false;
    }
    void stall() {
        std::lock_guard lock(mutex_);
        stall_ = true;
    }
    std::vector<http::Request> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    // (user message, system prompt) of each chat request.
    std::vector<std::pair<std::string, std::string>> calls() const {
        std::vector<std::pair<std::string, std::string>> found;
        for (const auto& request : requests()) {
            const Json messages = sent_messages(request);
            std::string system;
            std::string user;
            for (const auto& message : messages) {
                const std::string role = message.value("role", "");
                const std::string content = message.value("content", "");
                if (role == "system") {
                    system = content;
                } else if (role == "user") {
                    user = content;
                }
            }
            found.emplace_back(user, system);
        }
        return found;
    }

private:
    FakeClock& clock_;
    mutable std::mutex mutex_;
    std::vector<http::Request> requests_;
    std::string reply_ = "Rewritten.";
    bool stall_ = false;
};

inline Json command_settings() {
    Json settings = paste_settings();
    settings["recording_trigger_mode"] = "push_hold";
    settings["transcript_cleanup_provider"] = "openrouter";
    settings["transcript_cleanup_model"] = "test/model";
    return settings;
}

struct CommandRigOptions {
    Json settings = command_settings();
    // What the engine hears.
    std::string heard = "make it formal";
    // The provider's key is found (Command Mode and transforms are ready).
    bool provider_key = true;
    // The AI cleanup client's own key (a client without one is unavailable).
    bool cleanup_key = true;
    // The desktop's own clipboard and keys in place of the fakes (the [interactive] test); they
    // outlive the rig.
    focus::ClipboardPort* real_clipboard = nullptr;
    focus::SyntheticKeys* real_keys = nullptr;
};

// Both runtimes over the fakes, in a temporary data root.
struct CommandRig {
    explicit CommandRig(CommandRigOptions options = {})
        : settings(options.settings), recorder(root.path() / "recorded_audio.wav", trace),
          engine(std::make_shared<speech::FakeEngine>([&] {
              speech::FakeScript script;
              script.texts = {options.heard};
              return script;
          }())),
          speech(std::make_unique<speech::SpeechService>(engine->factory(), notify)),
          database(std::make_unique<Database>(Database::open_or_create(root.path() / "test.db"))),
          sync(trace),
          history(std::make_unique<HistoryManager>(*database, root.path() / "recordings",
                                                   RecordingRetention{},
                                                   HistoryServices{.record_sync = &sync})),
          stats(std::make_unique<dictation_stats::StatsRepository>(*database)),
          clipboard(trace, gui), keys(trace, gui), command_credentials(options.provider_key),
          cleanup_credentials(options.cleanup_key),
          model(std::make_shared<ModelClient>(model_clock)) {
        speech->select(speech::Slot::Dictation, fake_parakeet_config());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        while (speech->readiness(speech::Slot::Dictation).state == speech::ReadyState::Loading) {
            if (std::chrono::steady_clock::now() > deadline) {
                throw std::runtime_error("the fake engine did not load");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        TranscriptionServices services;
        services.settings = &settings;
        services.recorder = &recorder;
        services.speech = speech.get();
        services.history = history.get();
        services.stats = stats.get();
        focus::ClipboardPort& clipboard_port =
            options.real_clipboard != nullptr ? *options.real_clipboard : clipboard;
        focus::SyntheticKeys& keys_port = options.real_keys != nullptr ? *options.real_keys : keys;
        services.delivery.clipboard = &clipboard_port;
        services.delivery.keys = &keys_port;
        services.activity = &activity;
        services.ui = &gui;
        services.jobs = &jobs;
        services.persistence = &persistence;
        services.events = &events;
        services.capture = &capture;
        services.hooks.stop_playback = [] {
        };
        runtime = std::make_unique<TranscriptionRuntime>(std::move(services));
        links.push_back(runtime->status_update.connect(
            gui, [this](std::string status) { trace.add("status:" + status); }));
        links.push_back(
            runtime->overlay_state_update.connect(gui, [this](appcore::OverlayState state) {
                trace.add("overlay:" + std::string(appcore::to_string(state)));
            }));
        links.push_back(runtime->notice_requested.connect(gui, [this](std::string text, bool done) {
            trace.add("notice:" + text + (done ? " (done)" : ""));
        }));
        links.push_back(runtime->transcription_completed.connect(
            gui, [this](std::string text, std::optional<std::string> raw,
                        std::optional<CleanupInfo> info) {
                std::lock_guard lock(mutex);
                completions.push_back({std::move(text), std::move(raw), std::move(info)});
            }));
        llm::CleanupOptions cleanup_options;
        cleanup_options.clock = &model_clock;
        cleanup_options.default_prompt = text::legacy_default_prompt();
        cleanup = std::make_shared<llm::TranscriptCleanup>(
            model, cleanup_credentials.sources, [this] { return *settings.snapshot(); }, "", "", "",
            true, std::move(cleanup_options));
        commands = std::make_unique<CommandRuntime>(*runtime, settings, clipboard_port, keys_port,
                                                    capture, cleanup, gui, reader, system_clock(),
                                                    command_credentials.sources);
        runtime->set_command_runtime(commands.get());
    }
    ~CommandRig() {
        runtime->set_command_runtime(nullptr);
        commands.reset();
        reader.shutdown(std::chrono::seconds(5));
        links.clear();
        runtime.reset();
        cleanup.reset();
        speech.reset();
        stats.reset();
        history.reset();
        database->close();
    }
    CommandRig(const CommandRig&) = delete;
    CommandRig& operator=(const CommandRig&) = delete;

    // _settle: pumps the GUI thread until `done` holds (2 s at most).
    bool settle(const std::function<bool()>& done,
                std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            gui.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
    // Runs the jobs strand on a worker (a job may wait for a selection the GUI thread copies)
    // while the GUI thread pumps, then the History saves, until nothing is left.
    void drain() {
        for (;;) {
            std::size_t ran = gui.pump();
            if (jobs.queued() > 0) {
                auto worker = std::async(std::launch::async, [this] { return jobs.run_all(); });
                while (worker.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
                    gui.pump();
                }
                ran += worker.get();
            }
            ran += gui.pump();
            ran += persistence.run_all();
            ran += gui.pump();
            if (ran == 0) {
                return;
            }
        }
    }
    // Calls `fn` on a worker while the GUI thread pumps, as a job's worker would.
    template <class Fn> auto on_worker(Fn&& fn) {
        auto worker = std::async(std::launch::async, std::forward<Fn>(fn));
        while (worker.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
            gui.pump();
        }
        return worker.get();
    }

    // The recording's job, from its start until a stop claims the slot.
    std::shared_ptr<const DictationJob> recording_job() const {
        return runtime->job_slot().recording_job();
    }
    std::vector<std::string> statuses() const { return trace.with_prefix("status:"); }
    std::string last_status() const {
        const auto all = statuses();
        return all.empty() ? std::string() : all.back();
    }
    std::vector<std::string> notices() const { return trace.with_prefix("notice:"); }
    std::vector<std::string> overlays() const { return trace.with_prefix("overlay:"); }
    std::size_t pastes() const { return trace.count("paste"); }
    std::vector<std::string> copies() const { return trace.with_prefix("copy:"); }
    std::vector<std::string> stages() const { return trace.with_prefix("stage:"); }
    // The final pass of a recorded instruction for the job holding the slot, from the jobs
    // strand's side, then everything it queued.
    void transcribe() {
        const auto audio = wav("instruction.wav");
        on_worker([this, audio] { runtime->transcribe_audio_file(audio); });
        drain();
    }
    std::vector<HistoryEntry> entries() { return history->get_history(); }
    // Files kept in Recordings.
    std::size_t recordings() const {
        std::error_code error;
        std::size_t count = 0;
        for (const auto& item :
             std::filesystem::directory_iterator(root.path() / "recordings", error)) {
            count += item.is_regular_file() ? 1 : 0;
        }
        return count;
    }
    std::filesystem::path wav(const std::string& name) {
        const auto path = root.path() / name;
        std::filesystem::copy_file(audio_fixture("tone-16k-mono.wav"), path,
                                   std::filesystem::copy_options::overwrite_existing);
        return path;
    }
    struct Completion {
        std::string text;
        std::optional<std::string> raw;
        std::optional<CleanupInfo> info;
    };
    std::vector<Completion> completed() const {
        std::lock_guard lock(mutex);
        return completions;
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
    GuiExecutor gui;
    CommandClipboard clipboard;
    CommandSyntheticKeys keys;
    SwitchCapture capture;
    ActivityState activity;
    ManualExecutor jobs;
    ManualExecutor persistence;
    RecordingEvents events;
    TestCredentials command_credentials;
    TestCredentials cleanup_credentials;
    FakeClock model_clock;
    std::shared_ptr<ModelClient> model;
    std::shared_ptr<llm::TranscriptCleanup> cleanup;
    std::unique_ptr<TranscriptionRuntime> runtime;
    std::vector<Subscription> links;
    mutable std::mutex mutex;
    std::vector<Completion> completions;
    // Where CommandRuntime reads selections; destroyed before everything they use.
    ThreadPool reader{"fv-command-test", 2};
    std::unique_ptr<CommandRuntime> commands;
};

// A command job whose selection is known (Python's _job / _command_job); `focus` resolves to
// the snapshot when given.
inline std::shared_ptr<const DictationJob>
command_job(std::optional<std::string> selection,
            std::optional<focus::FocusSnapshot> focus = std::nullopt,
            JobMode mode = JobMode::Command) {
    DictationJob job;
    job.mode = mode;
    if (selection) {
        job.selection = as_future(std::move(*selection));
    }
    if (focus) {
        job.focus = focus::FocusRequest::resolved(std::move(*focus));
    }
    return std::make_shared<const DictationJob>(std::move(job));
}

} // namespace fv::dictation::testing
