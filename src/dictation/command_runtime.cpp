#include "dictation/command_runtime.h"

#include <exception>
#include <format>
#include <future>
#include <stdexcept>
#include <utility>
#include <variant>

#include "base/log.h"
#include "dictation/pipeline.h"
#include "dictation/transcription_runtime.h"
#include "focus/prompt_block.h"
#include "hotkeys/hold_machine.h"
#include "llm/rewrite.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "settings/store.h"

namespace fv::dictation {

namespace {

constexpr log::Logger kLog{"services.runtime.command"};

// How often a press waiting for the UI's answer checks that the runtime is still open.
constexpr std::chrono::milliseconds kAnswerPoll{20};

bool blank(std::string_view text) {
    return py::strip(text).empty();
}

} // namespace

CommandRuntime::CommandRuntime(TranscriptionRuntime& runtime, SettingsStore& settings,
                               focus::ClipboardPort& clipboard, focus::SyntheticKeys& keys,
                               focus::CaptureService& capture,
                               std::shared_ptr<llm::TranscriptCleanup> cleanup, Executor& ui,
                               Executor& jobs, Clock& clock, llm::KeySources credentials)
    : runtime_(runtime), settings_(settings), clipboard_(clipboard), keys_(keys), capture_(capture),
      cleanup_(std::move(cleanup)), ui_(ui), jobs_(jobs), clock_(clock), credentials_(credentials) {
}

CommandRuntime::~CommandRuntime() {
    cleanup();
    alive_.reset();
}

void CommandRuntime::cleanup() {
    closed_ = true;
    std::lock_guard lock(mutex_);
    recording_.reset();
}

void CommandRuntime::post_ui(std::function<void()> task) {
    std::weak_ptr<int> alive = alive_;
    ui_.post([alive, task = std::move(task)] {
        if (alive.lock()) {
            task();
        }
    });
}

void CommandRuntime::post_jobs(std::function<void()> task) {
    std::weak_ptr<int> alive = alive_;
    jobs_.post([alive, task = std::move(task)] {
        if (alive.lock()) {
            task();
        }
    });
}

bool CommandRuntime::call_on_ui(const std::function<bool()>& fn, bool fallback) {
    if (ui_.is_current()) {
        return fn();
    }
    auto promise = std::make_shared<std::promise<bool>>();
    std::future<bool> answer = promise->get_future();
    // The wait gives up once the runtime closes, so the task may run after it is gone, or after
    // the press it answers gave up: then it does nothing (no recording nobody would stop).
    std::weak_ptr<int> alive = alive_;
    ui_.post([this, promise, fn, alive, fallback] {
        try {
            const bool open = alive.lock() && !closed_.load();
            promise->set_value(open ? fn() : fallback);
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    while (answer.wait_for(kAnswerPoll) != std::future_status::ready) {
        if (closed_.load()) {
            return fallback;
        }
    }
    try {
        return answer.get();
    } catch (const std::future_error&) {
        return fallback; // the UI executor dropped the task (shutdown)
    }
}

// --- the shortcut ---------------------------------------------------------------------------

void CommandRuntime::key_pressed(double at) {
    if (closed_.load()) {
        return;
    }
    std::optional<Recording> recording;
    {
        std::lock_guard lock(mutex_);
        recording = recording_;
    }
    if (recording && running(*recording)) {
        if (!recording->push_hold) {
            stop(*recording);
        }
        return;
    }
    {
        std::lock_guard lock(mutex_);
        recording_.reset();
    }
    start(at);
}

void CommandRuntime::key_released(double at) {
    std::optional<Recording> recording;
    {
        std::lock_guard lock(mutex_);
        if (!recording_ || !recording_->push_hold) {
            return;
        }
        recording = std::move(recording_);
        recording_.reset();
    }
    if (!running(*recording)) {
        return;
    }
    if ((at - recording->press_at) * 1000.0 < hotkeys::kRecordMinHoldMs) {
        call_on_ui([this] { return runtime_.cancel(); }, false);
    } else {
        stop(*recording);
    }
}

bool CommandRuntime::running(const Recording& recording) const {
    // A Cancel or a failed capture ends the recording without a key event.
    return runtime_.is_recording() && runtime_.job_slot().recording_job() == recording.job;
}

void CommandRuntime::start(double at) {
    const Json settings = *settings_.snapshot();
    if (!llm::provider_ready(settings, credentials_)) {
        refuse(std::string(NO_PROVIDER_MESSAGE));
        return;
    }
    // A stop's post-roll also keeps the recorder running; the start below reports that job as
    // busy itself.
    if (runtime_.is_recording() && !runtime_.has_active_job()) {
        refuse("Finish the current recording before using Command Mode");
        return;
    }
    std::shared_ptr<focus::FocusRequest> probe;
    if (!keys::app_context_enabled.read(settings)) {
        // Before the start, so the capture still sees the app the user is in rather than our
        // overlay.
        probe = request_focus();
    }
    auto source = std::make_shared<SelectionSource>();
    StartOptions options;
    options.mode = JobMode::Command;
    options.selection = source->future();
    options.source = "hotkey";
    if (!call_on_ui([this, options] { return runtime_.start_recording(options); }, false)) {
        return;
    }
    const auto job = runtime_.job_slot().recording_job();
    const std::string id = job ? job->id : std::string();
    std::lock_guard lock(mutex_);
    // Only the recording just started and the job holding the slot can still read theirs.
    const auto active = runtime_.job_slot().active_job();
    for (auto it = reads_.begin(); it != reads_.end();) {
        it = it->first == id || (active && it->first == active->id) ? std::next(it)
                                                                    : reads_.erase(it);
    }
    reads_[id] = SelectionRead{source, job && job->focus ? job->focus : probe, false};
    recording_ =
        Recording{source, job, at, keys::recording_trigger_mode.read(settings) == "push_hold"};
}

void CommandRuntime::refuse(const std::string& message) {
    runtime_.status_update.emit(message);
    post_ui([this, message] { runtime_.notice_requested.emit(message, false); });
}

void CommandRuntime::stop(const Recording& recording) {
    {
        std::lock_guard lock(mutex_);
        recording_.reset();
    }
    call_on_ui(
        [this] {
            runtime_.stop_recording();
            return true;
        },
        false);
    read_selection_once(recording.job ? recording.job->id : std::string());
}

void CommandRuntime::read_selection_once(const std::string& job_id) {
    std::shared_ptr<SelectionSource> source;
    std::shared_ptr<focus::FocusRequest> focus;
    {
        std::lock_guard lock(mutex_);
        const auto found = reads_.find(job_id);
        if (found == reads_.end() || found->second.reading || found->second.source->done()) {
            return;
        }
        found->second.reading = true;
        source = found->second.source;
        focus = found->second.focus;
    }
    post_jobs([&keys = keys_, copy = clipboard_copy(clipboard_, keys_, ui_), focus, source] {
        read_selection(focus, keys, copy,
                       [source](SelectionValue value) { source->resolve(std::move(value)); });
    });
}

std::shared_ptr<focus::FocusRequest> CommandRuntime::request_focus() {
    try {
        return capture_.request(false, true);
    } catch (const std::exception&) {
        kLog.warning("Focus capture could not start");
        return nullptr;
    }
}

// --- transforms -----------------------------------------------------------------------------

void CommandRuntime::run_transform(std::string transform_id) {
    if (closed_.load()) {
        return;
    }
    const Json settings = *settings_.snapshot();
    auto transform = text::find_transform(settings, transform_id);
    if (!transform) {
        refuse("That transform no longer exists");
        return;
    }
    // The provider check reads keys, which never happens on the UI thread. Only references that
    // outlive the runtime are used while it blocks.
    std::weak_ptr<int> alive = alive_;
    post_jobs([this, alive, &ui = ui_, credentials = credentials_,
               transform = std::move(*transform), settings]() mutable {
        std::optional<bool> ready;
        try {
            ready = llm::provider_readiness(settings, credentials);
        } catch (const std::exception&) {
            kLog.warning("AI cleanup readiness unavailable");
            ready = false;
        }
        ui.post([this, alive, transform = std::move(transform), ready, settings] {
            if (alive.lock()) {
                begin_transform(transform, ready, settings);
            }
        });
    });
}

void CommandRuntime::begin_transform(const text::Transform& transform, std::optional<bool> ready,
                                     const Json& settings) {
    if (closed_.load()) {
        return;
    }
    // nullopt: an installed agent is still being looked up; the job checks again.
    if (ready.has_value() && !*ready) {
        refuse("Set up AI cleanup to use transforms");
        return;
    }
    const auto focus = request_focus();
    auto source = std::make_shared<SelectionSource>();
    DictationJob job;
    job.mode = JobMode::Transform;
    // Without app context the capture only finds the selection and where to paste; the job's
    // focus, and so History, does not learn the app.
    job.focus = keys::app_context_enabled.read(settings) ? focus : nullptr;
    job.selection = source->future();
    job.target = focus;
    // Transforms run only from their shortcuts.
    job.source = "hotkey";
    auto shared = std::make_shared<const DictationJob>(std::move(job));
    if (!runtime_.begin_rewrite_job(shared, std::format("Transform · {}", transform.name))) {
        return;
    }
    runtime_.status_update.emit(std::format("Rewriting · {}...", transform.name));
    std::weak_ptr<int> alive = alive_;
    post_jobs([this, alive, &ui = ui_, &keys = keys_, copy = clipboard_copy(clipboard_, keys_, ui_),
               focus, source, shared, instruction = transform.instruction] {
        read_selection(focus, keys, copy,
                       [this, alive, &ui, source, shared, instruction](SelectionValue value) {
                           ui.post([this, alive, source, shared, instruction,
                                    value = std::move(value)]() mutable {
                               if (alive.lock()) {
                                   transform_selected(shared, source, instruction,
                                                      std::move(value));
                               }
                           });
                       });
    });
}

void CommandRuntime::transform_selected(const std::shared_ptr<const DictationJob>& job,
                                        const std::shared_ptr<SelectionSource>& source,
                                        const std::string& instruction, SelectionValue value) {
    source->resolve(value);
    if (const auto* unread = std::get_if<UnreadSelection>(&value)) {
        runtime_.abandon_rewrite_job(unread->message);
        return;
    }
    std::string text = std::get<std::string>(std::move(value));
    if (blank(text)) {
        runtime_.abandon_rewrite_job("Select text to transform");
        return;
    }
    std::weak_ptr<int> alive = alive_;
    runtime_.submit_rewrite([this, alive, text = std::move(text), instruction, job] {
        const auto held = alive.lock();
        if (!held) {
            // Shut down between the submit and the jobs strand reaching the rewrite.
            throw std::runtime_error("Command Mode isn't available");
        }
        return rewrite(text, instruction, job);
    });
}

// --- rewriting ------------------------------------------------------------------------------

RewriteResult CommandRuntime::rewrite(const std::string& selection, const std::string& instruction,
                                      const std::shared_ptr<const DictationJob>& job) {
    const Json settings = *settings_.snapshot();
    if (!cleanup_) {
        throw CommandRefused(std::string(NO_PROVIDER_MESSAGE));
    }
    // This client processes selected text through the configured model endpoint.
    llm::configure_cleaner(*cleanup_, settings);
    if (!cleanup_->is_available()) {
        throw CommandRefused(std::string(NO_PROVIDER_MESSAGE));
    }
    std::string context_block;
    if (job && keys::app_context_read_text.read(settings)) {
        try {
            context_block = focus::prompt_block(job->snapshot(focus::CONTEXT_CAPTURE_DEADLINE));
        } catch (const std::exception&) {
            kLog.warning("Caret context unavailable");
        }
    }
    const auto started = clock_.steady();
    auto [result, error] =
        llm::rewrite_with(*cleanup_, selection, instruction, llm::dictionary_block(settings),
                          context_block, runtime_.job_slot().cancel_token());
    if (error) {
        if (llm::is_refusal(*error)) {
            throw CommandRefused(*error);
        }
        throw std::runtime_error(*error);
    }
    CleanupInfo info;
    info.provider = cleanup_->provider();
    info.model = cleanup_->model();
    info.elapsed_s = std::chrono::duration<double>(clock_.steady() - started).count();
    info.level = "";
    return RewriteResult{std::move(result),
                         blank(selection) ? std::nullopt : std::optional<std::string>(selection),
                         std::move(info)};
}

RewriteResult CommandRuntime::rewrite_text(const std::string& text,
                                           const std::string& instruction) {
    return rewrite(text, instruction, nullptr);
}

RewriteResult CommandRuntime::complete_recording(const std::string& raw,
                                                 const std::shared_ptr<const DictationJob>& job) {
    const Json settings = *settings_.snapshot();
    const std::string instruction =
        py::strip(prepare_text(raw, job.get(), settings, runtime_.text_preparer()).text);
    if (instruction.empty()) {
        throw CommandRefused(std::string(llm::kNoInstructionMessage));
    }
    std::string selection;
    if (job && job->selection.valid()) {
        read_selection_once(job->id);
        selection = job->selection_text(std::chrono::milliseconds(selection_wait_ms_.load()));
        const auto value = resolved(job->selection, std::chrono::milliseconds::zero());
        if (!value) {
            throw CommandRefused("Couldn't read the selected text. Try again.");
        }
        if (const auto* unread = std::get_if<UnreadSelection>(&*value)) {
            // Writing at the cursor would replace a selection still there.
            throw CommandRefused(unread->message);
        }
    }
    const bool writing = blank(selection);
    if (writing && !keys::command_mode_insert_without_selection.read(settings)) {
        throw CommandRefused("Select text first");
    }
    runtime_.overlay_state_update.emit(appcore::OverlayState::Rewriting);
    runtime_.status_update.emit(writing ? std::string("Writing...") : std::string("Rewriting..."));
    return rewrite(selection, instruction, job);
}

} // namespace fv::dictation
