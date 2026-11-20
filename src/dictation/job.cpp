#include "dictation/job.h"

#include <algorithm>
#include <future>

#include "base/ids.h"
#include "base/log.h"
#include "settings/keys.h"

namespace fv::dictation {

namespace {

constexpr log::Logger kLog{"services.dictation_pipeline"};

struct ModeName {
    JobMode mode;
    std::string_view name;
};

constexpr ModeName kModes[] = {
    {JobMode::Dictation, "dictation"},     {JobMode::Command, "command"},
    {JobMode::Transform, "transform"},     {JobMode::Action, "action"},
    {JobMode::Walkthrough, "walkthrough"},
};

} // namespace

std::string_view to_string(JobMode mode) {
    for (const auto& entry : kModes) {
        if (entry.mode == mode) {
            return entry.name;
        }
    }
    return "dictation";
}

std::optional<JobMode> job_mode_from(std::string_view value) {
    for (const auto& entry : kModes) {
        if (entry.name == value) {
            return entry.mode;
        }
    }
    return std::nullopt;
}

bool user_started(std::string_view source) {
    return source == "hotkey" || source == "ui" || source == "dbus";
}

SelectionFuture as_future(std::string text) {
    std::promise<SelectionValue> promise;
    promise.set_value(SelectionValue{std::move(text)});
    return promise.get_future().share();
}

std::optional<SelectionValue> resolved(const SelectionFuture& future,
                                       std::chrono::milliseconds timeout) {
    if (!future.valid()) {
        return std::nullopt;
    }
    try {
        if (future.wait_for(std::max(timeout, std::chrono::milliseconds::zero())) !=
            std::future_status::ready) {
            // Not ready in time: the caller goes on without it.
            return std::nullopt;
        }
        return future.get();
    } catch (const std::future_error&) {
        // Abandoned (Python's CancelledError).
        return std::nullopt;
    } catch (...) {
        kLog.debug("Focus capture result unavailable");
        return std::nullopt;
    }
}

std::optional<focus::FocusSnapshot> resolved(const std::shared_ptr<focus::FocusRequest>& request,
                                             std::chrono::milliseconds timeout) {
    if (!request) {
        return std::nullopt;
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
        return request->try_get();
    }
    return request->wait_for(timeout);
}

bool DictationJob::program_words() const {
    return changed_by_program || !user_started(source);
}

std::optional<focus::FocusSnapshot>
DictationJob::snapshot(std::chrono::milliseconds timeout) const {
    return resolved(focus, timeout);
}

std::string DictationJob::selection_text(std::chrono::milliseconds timeout) const {
    const auto value = resolved(selection, timeout);
    if (value) {
        if (const auto* text = std::get_if<std::string>(&*value)) {
            return *text;
        }
    }
    return {};
}

std::string DictationJob::new_id() {
    return random_hex(16);
}

FocusRequests focus_requests(JobMode mode, const Json& settings, std::string_view source,
                             std::string_view route, bool include_selection,
                             focus::CaptureService* service) {
    FocusRequests requests;
    try {
        std::shared_ptr<focus::CaptureService> installed;
        if (service == nullptr) {
            installed = focus::get_service();
            service = installed.get();
        }
        if (keys::app_context_enabled.read(settings)) {
            const bool users = user_started(source);
            const bool include_text = users && keys::app_context_read_text.read(settings);
            bool wanted = false;
            switch (mode) {
            case JobMode::Command:
                wanted = true;
                break;
            case JobMode::Dictation:
                wanted = include_text;
                break;
            case JobMode::Action:
                wanted = include_selection;
                break;
            default:
                break;
            }
            requests.focus = service->request(include_text, users && wanted);
        } else if (mode != JobMode::Dictation || !route.empty()) {
            // A rewrite pastes even with auto-paste off, and an action's answer may be pasted
            // later (a dictation may become an action once routed), so each still learns where
            // it started (DictationJob::target).
            requests.target = service->request(false);
        }
    } catch (...) {
        kLog.warning("Focus capture could not start");
    }
    return requests;
}

DictationJob begin_job(JobMode mode, const Json& settings, RecognitionContext recognition,
                       JobOptions options, focus::CaptureService* service) {
    static const Json kEmpty = Json::object();
    const Json& values = settings.is_object() ? settings : kEmpty;
    DictationJob job;
    job.mode = mode;
    if (mode != JobMode::Walkthrough) {
        auto requests = focus_requests(mode, values, options.source, options.route,
                                       options.include_selection, service);
        job.focus = std::move(requests.focus);
        job.target = std::move(requests.target);
    }
    job.recognition = std::move(recognition);
    job.selection = std::move(options.selection);
    job.action_id = std::move(options.action_id);
    job.source = std::move(options.source);
    job.route = std::move(options.route);
    return job;
}

} // namespace fv::dictation
