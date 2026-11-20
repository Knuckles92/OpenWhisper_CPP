#include "meeting/runtime.h"

#include <exception>
#include <format>
#include <stdexcept>
#include <utility>

#include "appcore/activity.h"
#include "automation/event_bus.h"
#include "meeting/detail/engine_internal.h"
#include "meeting/detail/py_value.h"
#include "meeting/detail/runtime_internal.h"
#include "meeting/detail/runtime_url.h"
#include "meeting/runtime_messages.h"
#include "settings/keys.h"
#include "settings/store.h"

namespace fv::meeting {

namespace {

const log::Logger& logger = detail::runtime_logger;

} // namespace

namespace detail {

bool terminal_finalization(std::string_view status) {
    return status == "completed" || status == "disabled" || status == "unavailable" ||
           status == "failed";
}

Json public_payload(const Json& payload) {
    return payload.is_object() ? payload : Json::object();
}

std::string text_of(const Json& object, std::string_view key) {
    return str_or_empty(get(object, key));
}

} // namespace detail

// ---- pure helpers ----

std::string redact_meeting_url(std::string_view url) {
    if (url.empty()) {
        return {};
    }
    detail::UrlParts parts;
    try {
        parts = detail::urlsplit(url);
    } catch (const detail::UrlError&) {
        return "<redacted>";
    }
    if (parts.scheme.empty() || parts.netloc.empty()) {
        return "<redacted>";
    }
    return parts.scheme + "://" + parts.netloc + "/<redacted>";
}

std::string with_history_target(std::string_view url, std::string_view meeting_id,
                                std::string_view view) {
    detail::UrlParts parts = detail::urlsplit(url);
    // dict(parse_qsl(...)): a repeated name keeps its first position and its last value.
    std::vector<std::pair<std::string, std::string>> query;
    const auto set = [&query](const std::string& name, std::string value) {
        for (auto& entry : query) {
            if (entry.first == name) {
                entry.second = std::move(value);
                return;
            }
        }
        query.emplace_back(name, std::move(value));
    };
    for (auto& [name, value] : detail::parse_qsl(parts.query)) {
        set(name, std::move(value));
    }
    set("history", std::string(meeting_id));
    if (!view.empty()) {
        set("view", std::string(view));
    }
    parts.query = detail::urlencode(query);
    return detail::urlunsplit(parts);
}

std::string ended_message(const Json& payload, std::string_view status, bool finalizing) {
    if (finalizing) {
        return std::string(messages::kEndedFinalizing);
    }
    if (detail::truthy(detail::get(payload, "capture_error"))) {
        return std::string(messages::kEndedCaptureError);
    }
    if (status == "needs_recovery") {
        return std::string(messages::kEndedNeedsRecovery);
    }
    return std::string(messages::kEnded);
}

std::optional<std::string> recovered_follow_up_step(const Json& meeting, const Json& settings) {
    if (detail::truthy(detail::get(meeting, "spool_dir")) &&
        keys::meeting_end_redecode.read(settings)) {
        return "redecode";
    }
    if (!detail::truthy(detail::get(meeting, "cloud_enabled"))) {
        return std::nullopt;
    }
    if (keys::meeting_end_polish.read(settings)) {
        return "polish";
    }
    if (keys::meeting_end_report.read(settings)) {
        return "consolidation";
    }
    return std::nullopt;
}

// ---- construction ----

MeetingRuntime::MeetingRuntime(MeetingRuntimeDeps deps)
    : deps_(std::move(deps)),
      ui_(deps_.ui != nullptr ? *deps_.ui
                              : throw std::invalid_argument("MeetingRuntime needs a UI executor")),
      clock_(deps_.clock != nullptr ? *deps_.clock : system_clock()),
      events_(deps_.events != nullptr ? *deps_.events : null_event_sink()),
      intelligence_(deps_.intelligence ? deps_.intelligence : null_intelligence()),
      model_lease_(deps_.model_lease ? deps_.model_lease : std::make_shared<NullModelLease>()) {
    if (!deps_.repository) {
        throw std::invalid_argument("MeetingRuntime needs a meeting repository");
    }
    if (deps_.sources.spool_root.empty()) {
        deps_.sources.spool_root = deps_.meetings_root;
    }
    if (deps_.sources.clock == nullptr) {
        deps_.sources.clock = &clock_;
    }
    if (!deps_.rerun) {
        deps_.rerun = [](const std::string& meeting_id, const RerunRequest& request) {
            return meeting::rerun_finalization(meeting_id, request);
        };
    }
    if (!deps_.find_recoverable) {
        deps_.find_recoverable = [this](meeting_state::MeetingRepository& repository) {
            return find_recoverable_meetings(repository, clock_);
        };
    }
    if (!deps_.asr) {
        deps_.asr = default_asr_factory();
    }
    if (!deps_.finalize_recovered) {
        deps_.finalize_recovered = [this](meeting_state::MeetingRepository& repository,
                                          const Json& meeting, const std::string& language,
                                          ModelLease& lease) {
            FinalizeOptions options;
            options.asr_language = language;
            options.clock = &clock_;
            return finalize_meeting(repository, meeting, deps_.asr, lease, {}, shutdown_.token(),
                                    options);
        };
    }
    if (!deps_.engine_factory) {
        deps_.engine_factory = default_engine_factory();
    }
    if (!deps_.archive_factory) {
        deps_.archive_factory = default_archive_factory();
    }
}

MeetingRuntime::~MeetingRuntime() {
    alive_->close();
    shutdown_.cancel();
    std::vector<Thread> threads;
    {
        const std::lock_guard lock(threads_mutex_);
        threads.swap(threads_);
    }
    // The workers run members of this object, so each is waited for however long it takes.
    for (Thread& thread : threads) {
        if (thread.thread.joinable()) {
            thread.thread.join();
        }
    }
    std::map<const RuntimeEngine*, Subscription> listeners;
    std::vector<std::shared_ptr<RuntimeEngine>> engines;
    std::shared_ptr<RuntimeArchive> archive;
    {
        const std::lock_guard lock(mutex_);
        listeners.swap(listeners_);
        engines.push_back(std::move(engine_));
        for (auto& entry : background_engines_) {
            engines.push_back(std::move(entry.second));
        }
        background_engines_.clear();
        archive = std::move(archive_);
    }
    // Listeners end first: no event of a dying engine reaches this object.
    listeners.clear();
    archive.reset();
    engines.clear();
}

// ---- shared ----

Json MeetingRuntime::settings_snapshot() const {
    if (deps_.settings == nullptr) {
        return Json::object();
    }
    return *deps_.settings->snapshot();
}

void MeetingRuntime::spawn(std::string name, Task task) {
    if (deps_.spawn) {
        deps_.spawn(std::move(name), std::move(task));
        return;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread thread([name, done, task = std::move(task)] {
        detail::name_engine_thread(name);
        try {
            task();
        } catch (const std::exception& error) {
            logger.error_with(error, std::format("Meeting worker {} failed", name));
        } catch (...) {
            logger.error("Meeting worker {} failed", name);
        }
        done->store(true);
    });
    std::vector<Thread> finished;
    {
        const std::lock_guard lock(threads_mutex_);
        for (auto it = threads_.begin(); it != threads_.end();) {
            if (it->done->load()) {
                finished.push_back(std::move(*it));
                it = threads_.erase(it);
            } else {
                ++it;
            }
        }
        threads_.push_back(Thread{std::move(thread), std::move(done)});
    }
    for (Thread& old : finished) {
        old.thread.join();
    }
}

void MeetingRuntime::post_ui(Task task) {
    if (ui_.is_current()) {
        task();
        return;
    }
    ui_.post([alive = alive_, task = std::move(task)] { alive->run(task); });
}

void MeetingRuntime::sync_activity() {
    if (deps_.activity == nullptr) {
        return;
    }
    bool claimed = false;
    bool finishing = false;
    {
        const std::lock_guard lock(mutex_);
        claimed = is_claimed_locked();
        finishing = finalizing_ || !background_engines_.empty() || archive_starting_ ||
                    retry_meeting_id_.has_value();
    }
    deps_.activity->set_meeting_active(claimed);
    deps_.activity->set_meeting_finishing(finishing);
}

bool MeetingRuntime::engine_active(const std::shared_ptr<RuntimeEngine>& engine) const {
    if (!engine) {
        return false;
    }
    try {
        return engine->is_active();
    } catch (const std::exception& error) {
        logger.debug("Meeting engine activity check failed: {}", error.what());
        return false;
    }
}

// is_active() reads an atomic of the engine; it is the one engine call made under the lock, as
// Python's RLock-guarded properties do.
bool MeetingRuntime::is_active_locked() const {
    return engine_active(engine_);
}

bool MeetingRuntime::is_claimed_locked() const {
    return starting_ || controller_meeting_active_ || is_active_locked();
}

bool MeetingRuntime::is_active() const {
    std::shared_ptr<RuntimeEngine> engine;
    {
        const std::lock_guard lock(mutex_);
        engine = engine_;
    }
    return engine_active(engine);
}

bool MeetingRuntime::is_claimed() const {
    const std::lock_guard lock(mutex_);
    return is_claimed_locked();
}

bool MeetingRuntime::is_finalizing() const {
    const std::lock_guard lock(mutex_);
    return finalizing_;
}

double MeetingRuntime::monotonic_s() const {
    return std::chrono::duration<double>(clock_.steady().time_since_epoch()).count();
}

Json MeetingRuntime::clock_fields(const std::shared_ptr<RuntimeEngine>& engine) const {
    if (!engine) {
        throw std::logic_error("no meeting engine for the meeting clock");
    }
    // The main window replays the latest state into a tab built later, so the tab needs the
    // instant the seconds were read, not just the count.
    Json fields = Json::object();
    fields["elapsed_s"] = engine->meeting_time_s();
    fields["elapsed_at"] = monotonic_s();
    return fields;
}

void MeetingRuntime::refresh_past_meetings() {
    try {
        past_meetings_refresh_requested.emit();
    } catch (const std::exception& error) {
        logger.error_with(error, "Could not request a Past Meetings refresh");
    }
}

std::shared_ptr<ModelLease> MeetingRuntime::model_lease() const {
    return model_lease_;
}

void MeetingRuntime::release_local_engine() {
    {
        const std::lock_guard lock(mutex_);
        if (engine_released_) {
            return;
        }
        engine_released_ = true;
    }
    if (!acquire_model_lease(model_lease_.get())) {
        const std::lock_guard lock(mutex_);
        engine_released_ = false;
    }
}

void MeetingRuntime::restore_local_engine() {
    {
        const std::lock_guard lock(mutex_);
        if (!engine_released_) {
            return;
        }
        engine_released_ = false;
    }
    release_model_lease(model_lease_.get());
}

// ---- small public actions ----

void MeetingRuntime::setup() {
    spawn("meeting-recovery-scan", [this] { recovery_scan_worker(); });
    spawn("meeting-restore-finalization", [this] { restore_last_finalization_worker(); });
}

void MeetingRuntime::retry_recovery_scan() {
    spawn("meeting-recovery-retry", [this] { recovery_scan_worker(); });
}

void MeetingRuntime::toggle_from_hotkey() {
    if (is_active()) {
        end();
    } else if (!is_claimed()) {
        meeting_platform_ack_requested.emit();
    }
}

void MeetingRuntime::set_consent_gate(
    std::function<bool(bool cloud, std::function<void(bool)> answer)> gate) {
    const std::lock_guard lock(mutex_);
    consent_gate_ = std::move(gate);
}

void MeetingRuntime::copy_guest_link() {
    std::optional<std::string> url;
    {
        const std::lock_guard lock(mutex_);
        url = guest_url_;
    }
    if (!url || url->empty()) {
        meeting_status_update.emit(std::string(messages::kNoGuestLink));
        return;
    }
    meeting_guest_link_ready.emit(*url);
}

void MeetingRuntime::retry_insights() {
    retry_step("failed");
}

void MeetingRuntime::retry_speakers() {
    retry_step("speaker_id");
}

} // namespace fv::meeting
