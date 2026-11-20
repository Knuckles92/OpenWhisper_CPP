#pragma once

// The harness of OpenWhisper's action runtime tests (tests/test_automation_actions_runtime.py
// `h`): a fake ActionHost standing in for the UI controller and the transcription runtime's
// paste, saved actions in an in-memory settings store, an event bus of the test's own, a focus
// service whose current app the test sets, credentials in memory, and the blocking work
// (open_uri, the agent's handoff and ask, the signed POST, run_once) replaced by functions the
// test sets, as the Python tests monkeypatch them. The UI executor is a ManualExecutor the test
// runs on its own thread (Python's QCoreApplication.processEvents).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "automation/action_model.h"
#include "automation/action_runtime.h"
#include "automation/catalog.h"
#include "automation/event_bus.h"
#include "base/identity.h"
#include "base/testing/manual_executor.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "dictation/job.h"
#include "dictation/pipeline.h"
#include "focus/capture_service.h"
#include "focus/snapshot.h"
#include "focus/testing/fake_platform.h"
#include "settings/testing/memory_store.h"

namespace fv::automation::test {

using namespace std::chrono_literals;

inline focus::AppIdentity app_identity(std::string app_id, std::string name, std::uint32_t pid,
                                       std::string window) {
    focus::AppIdentity identity;
    identity.app_id = std::move(app_id);
    identity.name = std::move(name);
    identity.pid = pid;
    identity.window = std::move(window);
    return identity;
}

inline const focus::AppIdentity& outlook() {
    static const focus::AppIdentity identity = app_identity("outlook.exe", "Outlook", 10, "0x1");
    return identity;
}
inline const focus::AppIdentity& slack() {
    static const focus::AppIdentity identity = app_identity("slack.exe", "Slack", 11, "0x2");
    return identity;
}

// TextContext(selected=..., selection_known=..., ...).
inline focus::TextContext selection(std::string selected, bool known = true, bool blocked = false,
                                    std::string source = {}) {
    focus::TextContext context;
    context.selected = std::move(selected);
    context.selection_known = known;
    context.blocked = blocked;
    context.source = std::move(source);
    return context;
}

// Action(id, name, kind, params, enabled=...).
inline Action an_action(std::string id, std::string name, std::string_view kind,
                        Json params = Json::object(), bool enabled = true) {
    Action action;
    action.id = std::move(id);
    action.name = std::move(name);
    action.kind = std::string(kind);
    action.params = std::move(params);
    action.enabled = enabled;
    return action;
}

struct JobSpec {
    std::optional<focus::AppIdentity> identity = outlook();
    std::optional<focus::TextContext> text;
    std::string language = "en";
    std::string source = "hotkey";
    bool changed_by_program = false;
};

// _job: an Action job the user started from Outlook, its capture already done.
inline std::shared_ptr<const dictation::DictationJob> action_job(std::string action_id,
                                                                 JobSpec spec = {}) {
    auto job = std::make_shared<dictation::DictationJob>();
    job->mode = dictation::JobMode::Action;
    job->action_id = std::move(action_id);
    job->source = std::move(spec.source);
    job->changed_by_program = spec.changed_by_program;
    job->focus = focus::FocusRequest::resolved(focus::FocusSnapshot{spec.identity, spec.text});
    job->recognition = dictation::RecognitionContext{spec.language, {}};
    return job;
}

// DictationJob(mode=ACTION, action_id=..., source=...): no capture at all.
inline std::shared_ptr<const dictation::DictationJob> bare_job(std::string action_id,
                                                               std::string source = "hotkey") {
    auto job = std::make_shared<dictation::DictationJob>();
    job->mode = dictation::JobMode::Action;
    job->action_id = std::move(action_id);
    job->source = std::move(source);
    return job;
}

// The UI controller and the transcription runtime's paste, recording every call. A host the
// test marked deleted throws, as a destroyed Qt wrapper does.
// Affinity: called on the test's thread (the UI executor); read anywhere.
class FakeHost final : public ActionHost {
public:
    explicit FakeHost(focus::CaptureService& capture) : capture_(capture) {}

    void set_status(std::string text) override {
        std::lock_guard lock(mutex_);
        alive();
        statuses.push_back(std::move(text));
    }
    void show_notice(std::string text, bool done) override {
        std::lock_guard lock(mutex_);
        alive();
        threads.push_back(std::this_thread::get_id());
        notices.emplace_back(std::move(text), done);
    }
    bool show_in_scratchpad(std::string text) override {
        std::lock_guard lock(mutex_);
        alive();
        scratchpad.push_back(std::move(text));
        return true;
    }
    bool copy_to_clipboard(std::string text, std::string html) override {
        std::lock_guard lock(mutex_);
        alive();
        copied.emplace_back(std::move(text), std::move(html));
        return copy_ok;
    }
    bool paste_text_now(std::string text, const dictation::DictationJob* job) override {
        std::lock_guard lock(mutex_);
        alive();
        pasted.emplace_back(std::move(text), job);
        return paste_ok;
    }
    dictation::PasteTarget paste_target(const dictation::DictationJob& job) override {
        return dictation::paste_target(&job, &capture_);
    }
    void discard_clipboard_prefetch() override {
        std::lock_guard lock(mutex_);
        alive();
        ++discarded;
    }

    std::vector<std::string> notice_texts() const {
        std::lock_guard lock(mutex_);
        std::vector<std::string> texts;
        for (const auto& notice : notices) {
            texts.push_back(notice.first);
        }
        return texts;
    }
    std::vector<std::pair<std::string, bool>> notices_now() const {
        std::lock_guard lock(mutex_);
        return notices;
    }
    std::vector<std::pair<std::string, std::string>> copied_now() const {
        std::lock_guard lock(mutex_);
        return copied;
    }

    std::vector<std::string> statuses;
    std::vector<std::pair<std::string, bool>> notices;
    std::vector<std::pair<std::string, std::string>> copied;
    std::vector<std::string> scratchpad;
    std::vector<std::pair<std::string, const dictation::DictationJob*>> pasted;
    std::vector<std::thread::id> threads;
    int discarded = 0;
    bool copy_ok = true;
    bool paste_ok = true;
    bool deleted = false;

private:
    void alive() const {
        if (deleted) {
            throw std::runtime_error("wrapped C/C++ object of type UIController has been deleted");
        }
    }

    focus::CaptureService& capture_;
    mutable std::mutex mutex_;
};

// A function the test replaces while workers may call it.
template <class F> class Swappable {
public:
    void set(F fn) {
        std::lock_guard lock(mutex_);
        fn_ = std::move(fn);
    }
    F get() const {
        std::lock_guard lock(mutex_);
        return fn_;
    }

private:
    mutable std::mutex mutex_;
    F fn_;
};

// What the fake run_once was given.
struct ProgramCall {
    std::vector<std::string> argv;
    std::string input;
    double timeout_s = 0;
    std::map<std::string, std::string> env;
    std::thread::id thread;
    CancelToken cancel;
};

// A call that waits until the test releases it (at most 10 s), counting who got in.
class Hold {
public:
    void release() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }
    // Waits until released or `cancel` fires (polled every 10 ms); true when released.
    bool wait(const CancelToken& cancel = {}, std::chrono::milliseconds limit = 10s) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        std::unique_lock lock(mutex_);
        while (!open_) {
            if (cancel.cancelled() || std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            changed_.wait_for(lock, 10ms);
        }
        return true;
    }
    void enter() {
        std::lock_guard lock(mutex_);
        ++entered_;
    }
    int entered() const {
        std::lock_guard lock(mutex_);
        return entered_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
    int entered_ = 0;
};

// agents.ask that waits until released or canceled (BlockedAsk): a stopped answer for a cancel,
// else `answer` followed by the words when `echo`. Copies share one state, so a worker's copy
// outlives the test's.
class BlockedAsk {
public:
    explicit BlockedAsk(std::string answer = "the answer: ", bool echo = true)
        : state_(std::make_shared<State>()) {
        state_->answer = std::move(answer);
        state_->echo = echo;
    }

    agents::AskResult operator()(const std::string& text, const CancelToken& cancel) const {
        {
            std::lock_guard lock(state_->mutex);
            state_->calls.emplace_back(text, cancel);
        }
        if (!state_->gate.wait(cancel)) {
            return agents::AskResult{false, "", "Stopped asking Claude Code.", true};
        }
        return agents::AskResult{true, state_->answer + (state_->echo ? text : std::string()), "",
                                 false};
    }
    bool started(std::size_t count) const {
        std::lock_guard lock(state_->mutex);
        return state_->calls.size() >= count;
    }
    std::vector<std::pair<std::string, CancelToken>> calls() const {
        std::lock_guard lock(state_->mutex);
        return state_->calls;
    }
    void release() const { state_->gate.release(); }

private:
    struct State {
        Hold gate;
        std::string answer;
        bool echo = true;
        std::mutex mutex;
        std::vector<std::pair<std::string, CancelToken>> calls;
    };
    std::shared_ptr<State> state_;
};

// A blocking call that waits until released (at most 10 s), counting who got in (Blocked).
// Copies share one state.
class Blocking {
public:
    Blocking() : state_(std::make_shared<State>()) {}

    void enter_and_wait() const {
        {
            std::lock_guard lock(state_->mutex);
            ++state_->calls;
        }
        state_->hold.wait({}, std::chrono::seconds(10));
    }
    bool started(int count = 1) const {
        std::lock_guard lock(state_->mutex);
        return state_->calls >= count;
    }
    int calls() const {
        std::lock_guard lock(state_->mutex);
        return state_->calls;
    }
    void release() const { state_->hold.release(); }

private:
    struct State {
        std::mutex mutex;
        int calls = 0;
        Hold hold;
    };
    std::shared_ptr<State> state_;
};
struct HarnessOptions {
    std::shared_ptr<CredentialBackend> backend = std::make_shared<MemoryCredentialBackend>();
    // The pool the runtime's strand runs on; null: its own.
    ThreadPool* pool = nullptr;
    // Another executor for results than the harness's ManualExecutor (which still runs them).
    std::function<Executor&(ManualExecutor&)> ui;
};

// h: the runtime over the fakes.
struct Harness {
    explicit Harness(HarnessOptions options = {})
        : settings(Json{{"actions", Json::array()}}),
          capture(focus::FocusSnapshot{outlook(), std::nullopt},
                  std::optional<std::optional<focus::AppIdentity>>(outlook())),
          host(capture), backend(options.backend), credentials(backend, identity()) {
        ActionServices services;
        services.open_uri = [this](std::string_view uri) {
            const auto fn = open_uri.get();
            if (!fn) {
                throw std::logic_error("open_uri was not expected");
            }
            fn(std::string(uri));
        };
        services.handoff = [this](const agents::HandoffRequest& request) {
            const auto fn = handoff.get();
            if (!fn) {
                throw std::logic_error("handoff was not expected");
            }
            return fn(request);
        };
        services.ask = [this](std::string_view agent_id, std::string_view text,
                              std::string_view model, const CancelToken& cancel) {
            const auto fn = ask.get();
            if (!fn) {
                throw std::logic_error("ask was not expected");
            }
            return fn(std::string(agent_id), std::string(text), std::string(model), cancel);
        };
        services.post_signed_reply = [this](std::string_view url, const Json& payload,
                                            std::string_view secret, const SignedPost& sent) {
            const auto fn = post.get();
            if (!fn) {
                throw std::logic_error("post_signed_reply was not expected");
            }
            return fn(std::string(url), payload, std::string(secret), sent);
        };
        services.run_once = [this](const std::vector<std::string>& argv, std::string_view input,
                                   double timeout_s, const CancelToken& cancel,
                                   const std::map<std::string, std::string>& extra_env) {
            {
                std::lock_guard lock(mutex);
                programs.push_back(ProgramCall{argv, std::string(input), timeout_s, extra_env,
                                               std::this_thread::get_id(), cancel});
            }
            const auto fn = run_once.get();
            if (!fn) {
                throw std::logic_error("run_once was not expected");
            }
            return fn(argv, input, timeout_s, cancel, extra_env);
        };
        services.subscribers = [this](std::string_view label) {
            return bus.subscribers(label);
        };
        services.mcp_subscriptions = [this] {
            return mcp_count.load();
        };
        services.pool = options.pool;
        Executor& results = options.ui ? options.ui(ui) : ui;
        rt = std::make_unique<ActionRuntime>(host, settings, credentials, bus, results,
                                             std::move(services));
    }
    ~Harness() {
        rt.reset();
        bus.close();
    }
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    // h.add: saves the action after those already saved.
    void add(const Action& action) {
        settings.mutate([&](Json& all) { all["actions"].push_back(action.to_dict()); });
    }
    void set(std::string_view key, const Json& value) { settings.save_setting(key, value); }

    // Runs the UI executor until `predicate` holds; false when `timeout` passed first.
    bool pump(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 10s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            ui.run_all();
            if (predicate()) {
                return true;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(5ms);
        }
    }
    // Runs the UI executor for `duration`.
    void spin(std::chrono::milliseconds duration) {
        const auto until = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < until) {
            ui.run_all();
            std::this_thread::sleep_for(5ms);
        }
        ui.run_all();
    }

    // action.ran payloads, oldest first.
    std::vector<Json> ran() const { return of_type(catalog::ACTION_RAN); }
    std::vector<Json> of_type(std::string_view type) const {
        std::vector<Json> found;
        for (const Event& event : bus.recent(500, std::vector<std::string>{std::string(type)})) {
            found.push_back(event.data);
        }
        return found;
    }
    // _finished: the last action.ran once `count` were published.
    Json finished(std::size_t count = 1) {
        if (!pump([&] { return ran().size() >= count; })) {
            throw std::runtime_error("timed out waiting for action.ran");
        }
        return ran().back();
    }
    std::vector<ProgramCall> program_calls() const {
        std::lock_guard lock(mutex);
        return programs;
    }

    MemorySettingsStore settings;
    EventBus bus;
    ManualExecutor ui;
    focus::FakeCaptureService capture;
    FakeHost host;
    std::shared_ptr<CredentialBackend> backend;
    CredentialStore credentials;
    std::atomic<std::size_t> mcp_count{0};

    Swappable<std::function<void(const std::string&)>> open_uri;
    Swappable<std::function<std::string(const agents::HandoffRequest&)>> handoff;
    Swappable<std::function<agents::AskResult(const std::string& agent, const std::string& text,
                                              const std::string& model, const CancelToken&)>>
        ask;
    Swappable<std::function<std::pair<DeliveryRecord, std::optional<std::string>>(
        const std::string& url, const Json& payload, const std::string& secret,
        const SignedPost& options)>>
        post;
    Swappable<RunOnceFn> run_once;

    mutable std::mutex mutex;
    std::vector<ProgramCall> programs;

    std::unique_ptr<ActionRuntime> rt;
};

// A finished run_once: exit code, stdout and error.
inline RunOnceResult program_result(std::optional<int> exit_code, std::string output,
                                    std::string error = {}) {
    RunOnceResult result;
    result.exit_code = exit_code;
    result.output = std::move(output);
    result.error = std::move(error);
    result.canceled = result.error == fv::CANCELED;
    return result;
}

inline RunOnceFn answers(std::optional<int> exit_code, std::string output, std::string error = {}) {
    return [=](const std::vector<std::string>&, std::string_view, double, const CancelToken&,
               const std::map<std::string, std::string>&) {
        return program_result(exit_code, output, error);
    };
}

// DeliveryRecord("", "", "", ok, status, 3).
inline DeliveryRecord delivery(bool ok, std::optional<int> status) {
    DeliveryRecord record;
    record.ok = ok;
    record.status = status;
    record.elapsed_ms = 3;
    return record;
}

inline std::function<std::pair<DeliveryRecord, std::optional<std::string>>(
    const std::string&, const Json&, const std::string&, const SignedPost&)>
replies(DeliveryRecord record, std::optional<std::string> body) {
    return [=](const std::string&, const Json&, const std::string&, const SignedPost&) {
        return std::pair{record, body};
    };
}

inline agents::AskResult asked(bool ok, std::string answer, std::string message = {},
                               bool stopped = false) {
    return agents::AskResult{ok, std::move(answer), std::move(message), stopped};
}

inline ActionResult result(bool ok, std::string message, std::string output = {},
                           bool stopped = false) {
    return ActionResult{ok, std::move(message), std::move(output), stopped};
}

} // namespace fv::automation::test
