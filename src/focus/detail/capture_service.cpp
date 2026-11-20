#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <typeinfo>
#include <variant>

#include "base/log.h"
#include "base/metrics.h"
#include "focus/catalog.h"
#include "focus/detail/platform.h"
#include "focus/focus_capture.h"
#include "pyport/number.h"
#include "settings/keys.h"

namespace fv::focus {

namespace {

constexpr log::Logger kLog("services.focus_context._capture");

using SteadyTime = std::chrono::steady_clock::time_point;
using Callback = std::function<void(std::optional<TextContext>)>;

// The exception's type alone: its text could quote the app's content.
std::string error_type(const std::exception& error) {
    std::string name = typeid(error).name();
    for (std::string_view prefix : {"class ", "struct "}) {
        if (name.starts_with(prefix)) {
            name.erase(0, prefix.size());
        }
    }
    return name;
}

struct ReadTask {
    AppIdentity identity;
    bool include_text = false;
    bool include_selection = false;
    std::shared_ptr<FocusRequest> request;
    SteadyTime started;
    std::shared_ptr<TimerHandle> timer;
};

struct RereadTask {
    AppIdentity identity;
    Callback callback;
};

using Task = std::variant<ReadTask, RereadTask>;

const AppIdentity& task_identity(const Task& task) {
    return std::visit([](const auto& item) -> const AppIdentity& { return item.identity; }, task);
}

// The thread that owns one text reader (and its COM apartment).
struct Worker {
    std::mutex queue_mutex;
    std::condition_variable queued;
    // nullopt asks the thread to stop.
    std::deque<std::optional<Task>> queue;

    // Guarded by Core::mutex.
    std::optional<SteadyTime> busy_since;
    std::string task_app;
    bool abandoned = false;

    void push(std::optional<Task> task) {
        {
            std::lock_guard lock(queue_mutex);
            queue.push_back(std::move(task));
        }
        queued.notify_one();
    }

    std::optional<Task> pop() {
        std::unique_lock lock(queue_mutex);
        queued.wait(lock, [this] { return !queue.empty(); });
        std::optional<Task> task = std::move(queue.front());
        queue.pop_front();
        return task;
    }
};

void deliver(const Callback& callback, std::optional<TextContext> context) {
    try {
        callback(std::move(context));
    } catch (const std::exception& error) {
        kLog.warning("Text re-read callback failed ({})", error_type(error));
    } catch (...) {
        kLog.warning("Text re-read callback failed");
    }
}

void record_default(const CaptureSample& sample) {
    metrics::record("context_capture_ms", sample.context_capture_ms);
    metrics::record("context_capture_timeouts", sample.context_capture_timeouts);
}

} // namespace

struct FocusCapture::Core : std::enable_shared_from_this<Core> {
    std::shared_ptr<FocusPlatform> platform;
    // The threads keep Core alive past the service (an abandoned read never returns), but the
    // settings and metrics providers may refer to objects their owner destroys right after
    // shutdown(), such as the SettingsStore. shutdown() drops both under the exclusive lock, so
    // once it returns no call into them starts or is still running. A call holds the shared lock
    // only while it runs (an in-memory snapshot or a metric), never while reading an app's text.
    std::shared_mutex callouts_mutex;
    std::function<std::shared_ptr<const Json>()> settings;
    std::function<void(const CaptureSample&)> metrics;
    const Clock* clock;
    std::atomic<std::int64_t> deadline_ms;
    std::chrono::milliseconds wedged_after;
    std::atomic<bool> closed{false};

    // Guards timers alone, so FocusCapture's destructor can take the executor away before the
    // pool it may own goes.
    std::mutex timers_mutex;
    Executor* timers = nullptr;

    mutable std::mutex mutex;
    std::shared_ptr<Worker> worker;
    int workers_started = 0;
    int replacements = 0;
    bool text_off = false;
    std::set<std::string> disabled;
    std::map<std::string, int, std::less<>> misses;
    std::vector<AppIdentity> recent;

    std::chrono::milliseconds deadline() const {
        return std::chrono::milliseconds(deadline_ms.load());
    }

    // A pending timer, or nullptr once the executor is gone.
    std::shared_ptr<TimerHandle> start_timer(std::chrono::milliseconds delay,
                                             std::function<void()> task) {
        std::lock_guard lock(timers_mutex);
        if (timers == nullptr) {
            return nullptr;
        }
        return std::make_shared<TimerHandle>(
            timers->post_after(std::max(delay, std::chrono::milliseconds(0)), std::move(task)));
    }

    std::optional<AppIdentity> identity() {
        try {
            return platform->identity();
        } catch (const std::exception& error) {
            kLog.debug("Focused app unavailable ({})", error_type(error));
        } catch (...) {
            kLog.debug("Focused app unavailable");
        }
        return std::nullopt;
    }

    void record(const CaptureSample& sample) {
        std::string failure;
        bool failed = false;
        {
            std::shared_lock lock(callouts_mutex);
            if (!metrics) {
                return; // Shut down.
            }
            try {
                metrics(sample);
            } catch (const std::exception& error) {
                failed = true;
                failure = error_type(error);
            } catch (...) {
                failed = true;
            }
        }
        if (failed) {
            if (failure.empty()) {
                kLog.debug("Could not record capture metrics");
            } else {
                kLog.debug("Could not record capture metrics ({})", failure);
            }
        }
    }

    // Stops every later call into settings and metrics, waiting for one in flight.
    void drop_callouts() {
        std::unique_lock lock(callouts_mutex);
        settings = nullptr;
        metrics = nullptr;
    }

    void remember(const std::optional<AppIdentity>& identity) {
        if (!identity || identity->is_self) {
            return;
        }
        std::lock_guard lock(mutex);
        std::vector<AppIdentity> updated{*identity};
        for (const auto& seen : recent) {
            if (updated.size() >= RECENT_APPS) {
                break;
            }
            if (seen.app_id != identity->app_id || seen.title_hint != identity->title_hint) {
                updated.push_back(seen);
            }
        }
        recent = std::move(updated);
    }

    // Whether identity is an app the user excluded from reading. Settings that cannot be read
    // fail closed.
    bool excluded(const AppIdentity& identity) {
        Json values;
        std::string failure;
        bool failed = false;
        {
            std::shared_lock lock(callouts_mutex);
            if (!settings) {
                return true; // Shut down: nothing more is read.
            }
            try {
                const auto saved = settings();
                values = keys::app_context_excluded_apps.read(saved ? *saved : Json::object());
            } catch (const std::exception& error) {
                failed = true;
                failure = error_type(error);
            } catch (...) {
                failed = true;
            }
        }
        if (failed) {
            if (failure.empty()) {
                kLog.warning("Excluded apps unavailable; not reading text near the cursor");
            } else {
                kLog.warning("Excluded apps unavailable; not reading text near the cursor ({})",
                             failure);
            }
            return true;
        }
        if (!values.is_array()) {
            return false;
        }
        return std::ranges::any_of(values, [&](const Json& value) {
            return value.is_string() &&
                   catalog::matches(identity, value.get_ref<const std::string&>());
        });
    }

    bool refuses(const std::optional<AppIdentity>& identity) {
        if (!identity || identity->is_self || identity->app_id.empty()) {
            return false;
        }
        return excluded(*identity);
    }

    bool text_allowed(const std::optional<AppIdentity>& identity) {
        if (!identity || identity->is_self || identity->app_id.empty()) {
            return false;
        }
        if (!platform->text_supported() || catalog::is_terminal(identity)) {
            return false;
        }
        std::lock_guard lock(mutex);
        return !text_off && !disabled.contains(identity->app_id);
    }

    void start_worker_thread(const std::shared_ptr<Worker>& started) {
        std::thread([self = shared_from_this(), started] { self->run(started); }).detach();
    }

    // Hands task to the text thread; false when it cannot take it now.
    bool submit(Task task) {
        std::string abandoned_app;
        bool abandoned = false;
        bool now_off = false;
        bool accepted = false;
        {
            std::lock_guard lock(mutex);
            if (closed || text_off) {
                return false;
            }
            const std::string app = task_identity(task).app_id;
            auto current = worker;
            bool refused = false;
            if (current && current->busy_since) {
                if (clock->steady() - *current->busy_since < wedged_after) {
                    return false;
                }
                // The stuck call cannot be cancelled; its thread exits if the call ever returns.
                current->abandoned = true;
                current->push(std::nullopt);
                if (!current->task_app.empty()) {
                    disabled.insert(current->task_app);
                }
                abandoned = true;
                abandoned_app = current->task_app;
                worker = nullptr;
                ++replacements;
                if (replacements > MAX_WORKER_REPLACEMENTS) {
                    text_off = true;
                }
                now_off = text_off;
                current = nullptr;
                refused = text_off || disabled.contains(app);
            }
            if (!refused) {
                if (!current) {
                    current = worker = std::make_shared<Worker>();
                    ++workers_started;
                    start_worker_thread(current);
                }
                current->busy_since = clock->steady();
                current->task_app = app;
                current->push(std::move(task));
                accepted = true;
            }
        }
        if (abandoned) {
            kLog.warning("Text near the cursor stopped answering in {}; not reading it there "
                         "again this session{}",
                         abandoned_app.empty() ? std::string("an app") : abandoned_app,
                         now_off ? " (text capture is now off)" : "");
        }
        return accepted;
    }

    void expire(const AppIdentity& identity, const std::shared_ptr<FocusRequest>& request) {
        // The miss is counted before the request resolves, so whoever it wakes sees it.
        if (request->settle(FocusSnapshot{identity, std::nullopt},
                            [&] { note_miss(identity.app_id); })) {
            record(CaptureSample{static_cast<double>(deadline().count()), 1});
        }
    }

    void note_miss(const std::string& app_id) {
        bool now_disabled = false;
        {
            std::lock_guard lock(mutex);
            const int count = ++misses[app_id];
            if (count >= MAX_CONSECUTIVE_MISSES && !disabled.contains(app_id)) {
                disabled.insert(app_id);
                now_disabled = true;
            }
        }
        if (now_disabled) {
            kLog.info("Text near the cursor is too slow in {}; not reading it there again this "
                      "session",
                      app_id);
        }
    }

    void continue_capture(const std::shared_ptr<FocusRequest>& request,
                          const std::optional<AppIdentity>& identity, bool include_text,
                          bool include_selection, SteadyTime started) {
        remember(identity);
        if (include_selection && refuses(identity)) {
            // Checked before any read, even where text can't be read at all, so a busy or slow
            // reader can't turn the refusal into "unknown", which Command Mode would answer with
            // a copy.
            request->settle(FocusSnapshot{
                identity, TextContext{.source = std::string(EXCLUDED_SOURCE), .blocked = true}});
            return;
        }
        if (!(include_text || include_selection) || !text_allowed(identity)) {
            request->settle(FocusSnapshot{identity, std::nullopt});
            return;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline() - (clock->steady() - started));
        std::weak_ptr<Core> weak = weak_from_this();
        auto timer = start_timer(remaining, [weak, identity = *identity, request] {
            if (auto self = weak.lock()) {
                self->expire(identity, request);
            }
        });
        if (!timer) {
            request->settle(FocusSnapshot{identity, std::nullopt});
            return;
        }
        ReadTask task{*identity, include_text, include_selection, request, started, timer};
        if (!submit(std::move(task))) {
            timer->cancel();
            request->settle(FocusSnapshot{identity, std::nullopt});
        }
    }

    void identify_async(const std::shared_ptr<FocusRequest>& request, bool include_text,
                        bool include_selection, SteadyTime started) {
        auto timer = start_timer(deadline(), [request] { request->settle(FocusSnapshot{}); });
        if (!timer) {
            request->settle(FocusSnapshot{});
            return;
        }
        // A detached thread: a hung hyprctl or X server only costs the caller the deadline.
        std::thread([self = shared_from_this(), request, include_text, include_selection, started,
                     timer] {
            detail::set_thread_name("fv-focus-identity");
            const auto identity = self->identity();
            if (!request->ready()) {
                self->continue_capture(request, identity, include_text, include_selection, started);
            }
        }).detach();
    }

    std::unique_ptr<TextReader> open_reader() {
        try {
            return platform->text_reader();
        } catch (const std::exception& error) {
            {
                std::lock_guard lock(mutex);
                text_off = true;
            }
            kLog.warning("Text near the cursor is unavailable on this computer ({})",
                         error_type(error));
        } catch (...) {
            {
                std::lock_guard lock(mutex);
                text_off = true;
            }
            kLog.warning("Text near the cursor is unavailable on this computer");
        }
        return nullptr;
    }

    std::optional<TextContext> read(TextReader* reader, const AppIdentity& identity,
                                    bool include_text, bool include_selection) {
        if (reader == nullptr || excluded(identity)) {
            return std::nullopt;
        }
        try {
            return reader->read(identity, include_text, include_selection);
        } catch (const std::exception& error) {
            kLog.debug("Reading text near the cursor failed ({})", error_type(error));
        } catch (...) {
            kLog.debug("Reading text near the cursor failed");
        }
        return std::nullopt;
    }

    // Runs task on the text thread. The worker is marked idle once the reader has answered,
    // before the request resolves or the callback runs, so a caller that asks again as soon as
    // it has its answer is read again rather than turned away as busy; finished_early says so.
    void execute(Task& task, TextReader* reader, Worker& self_worker, bool& finished_early) {
        if (auto* reread = std::get_if<RereadTask>(&task)) {
            std::optional<TextContext> context;
            const auto current = identity();
            // The site matters too: site exclusions match on it.
            if (same_target(current, reread->identity)) {
                context = read(reader, *current, true, true);
            }
            finished(self_worker);
            finished_early = true;
            deliver(reread->callback, std::move(context));
            return;
        }
        auto& job = std::get<ReadTask>(task);
        auto context = read(reader, job.identity, job.include_text, job.include_selection);
        finished(self_worker);
        finished_early = true;
        if (job.timer) {
            job.timer->cancel();
        }
        // The miss count is reset before the request resolves: a caller that asks again as
        // soon as it has this answer must not be judged on the misses before it.
        if (job.request->settle(FocusSnapshot{job.identity, std::move(context)}, [&] {
                std::lock_guard lock(mutex);
                misses.erase(job.identity.app_id);
            })) {
            const double elapsed =
                std::chrono::duration<double, std::milli>(clock->steady() - job.started).count();
            record(CaptureSample{py::round_ndigits(elapsed, 1), 0});
        }
    }

    void finished(Worker& done) {
        std::lock_guard lock(mutex);
        done.busy_since.reset();
        done.task_app.clear();
    }

    void run(const std::shared_ptr<Worker>& self_worker) {
        detail::set_thread_name("fv-focus");
        std::unique_ptr<TextReader> reader = open_reader();
        for (;;) {
            std::optional<Task> task = self_worker->pop();
            if (!task) {
                break;
            }
            bool finished_early = false;
            try {
                execute(*task, reader.get(), *self_worker, finished_early);
            } catch (const std::exception& error) {
                // Exception text could quote the app's content; the type cannot.
                kLog.warning("Text capture task failed ({})", error_type(error));
            } catch (...) {
                kLog.warning("Text capture task failed");
            }
            if (!finished_early) {
                finished(*self_worker);
            }
            bool stop = false;
            {
                std::lock_guard lock(mutex);
                stop = self_worker->abandoned;
            }
            if (stop) {
                break;
            }
        }
        try {
            reader.reset();
        } catch (...) {
            kLog.debug("Text reader did not close cleanly");
        }
    }
};

FocusCapture::FocusCapture(std::shared_ptr<FocusPlatform> platform, FocusCaptureOptions options)
    : core_(std::make_shared<Core>()) {
    core_->platform = std::move(platform);
    core_->settings = options.settings ? std::move(options.settings) : [] {
        return std::make_shared<const Json>(Json::object());
    };
    core_->metrics = options.metrics ? std::move(options.metrics) : record_default;
    core_->clock = options.clock != nullptr ? options.clock : &system_clock();
    core_->deadline_ms = options.deadline.count();
    core_->wedged_after = options.wedged_after;
    if (options.timers != nullptr) {
        core_->timers = options.timers;
    } else {
        own_timers_ = std::make_unique<ThreadPool>("fv-focus-deadline", 1);
        core_->timers = own_timers_.get();
    }
}

FocusCapture::~FocusCapture() {
    shutdown();
    {
        std::lock_guard lock(core_->timers_mutex);
        core_->timers = nullptr;
    }
    own_timers_.reset();
}

std::shared_ptr<FocusRequest> FocusCapture::request(bool include_text, bool include_selection) {
    const SteadyTime started = core_->clock->steady();
    auto request = std::make_shared<FocusRequest>();
    try {
        if (core_->closed) {
            request->settle(FocusSnapshot{});
        } else if (core_->platform->sync_identity()) {
            core_->continue_capture(request, core_->identity(), include_text, include_selection,
                                    started);
        } else {
            core_->identify_async(request, include_text, include_selection, started);
        }
    } catch (const std::exception& error) {
        kLog.warning("Focus capture failed ({})", error_type(error));
        request->settle(FocusSnapshot{});
    }
    return request;
}

std::optional<AppIdentity> FocusCapture::current_identity() {
    if (core_->closed || !core_->platform->sync_identity()) {
        return std::nullopt;
    }
    return core_->identity();
}

std::optional<AppIdentity> FocusCapture::identity_now(std::chrono::milliseconds timeout) {
    if (core_->closed) {
        return std::nullopt;
    }
    if (core_->platform->sync_identity()) {
        return core_->identity();
    }
    assert_not_gui_thread();
    auto answer = std::make_shared<FocusRequest>();
    // A detached thread, as in request(): a hung hyprctl or X server only costs the caller the
    // timeout.
    std::thread([core = core_, answer] {
        detail::set_thread_name("fv-focus-identity");
        answer->settle(FocusSnapshot{core->identity(), std::nullopt});
    }).detach();
    if (auto snapshot = answer->wait_for(std::max(timeout, std::chrono::milliseconds(0)))) {
        return snapshot->identity;
    }
    kLog.debug("Focused app not named within {:.2f}s",
               std::chrono::duration<double>(timeout).count());
    return std::nullopt;
}

void FocusCapture::reread(const std::optional<AppIdentity>& identity,
                          std::function<void(std::optional<TextContext>)> callback) {
    try {
        if (core_->closed || !core_->platform->sync_identity() || !core_->text_allowed(identity) ||
            !core_->submit(RereadTask{*identity, callback})) {
            deliver(callback, std::nullopt);
        }
    } catch (const std::exception& error) {
        kLog.warning("Text re-read failed ({})", error_type(error));
        deliver(callback, std::nullopt);
    }
}

void FocusCapture::shutdown() {
    std::shared_ptr<Worker> worker;
    {
        std::lock_guard lock(core_->mutex);
        core_->closed = true;
        worker = std::exchange(core_->worker, nullptr);
    }
    if (worker) {
        worker->push(std::nullopt);
    }
    core_->drop_callouts();
}

std::vector<AppIdentity> FocusCapture::recent_apps() {
    std::lock_guard lock(core_->mutex);
    return core_->recent;
}

FocusCapture::State FocusCapture::state() const {
    std::lock_guard lock(core_->mutex);
    return State{
        .has_worker = core_->worker != nullptr,
        .worker_busy = core_->worker != nullptr && core_->worker->busy_since.has_value(),
        .workers_started = core_->workers_started,
        .replacements = core_->replacements,
        .text_off = core_->text_off,
        .disabled = core_->disabled,
        .misses = core_->misses,
    };
}

void FocusCapture::set_deadline(std::chrono::milliseconds deadline) {
    core_->deadline_ms = deadline.count();
}

} // namespace fv::focus
