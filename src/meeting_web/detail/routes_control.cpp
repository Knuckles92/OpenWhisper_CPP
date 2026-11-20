// api.py _register_note_routes (_log_slow_end) and _register_meeting_control_routes.

#include <chrono>
#include <format>
#include <string>
#include <utility>

#include "base/log.h"
#include "meeting_web/dashboard_api.h"
#include "meeting_web/detail/api_support.h"
#include "meeting_web/detail/py_value.h"
#include "meeting_web/extensions.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "settings/store.h"

namespace fv::meeting_web {

namespace {

constexpr log::Logger logger("meeting.web.api");

double seconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double>(duration).count();
}

} // namespace

void DashboardApi::note_request(const DashboardRequest& request, Reply reply) {
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    const std::string role = require(token);
    const Json body = json_body(request.http);
    const Json& text = detail::get(body, "text");
    if (!text.is_string() || py::strip(text.get<std::string>()).empty() ||
        static_cast<std::int64_t>(py::len(text.get<std::string>())) >
            detail::kNoteRequestMaxChars) {
        throw HttpError(400, std::format("Enter a note request of 1 to {} characters.",
                                         detail::kNoteRequestMaxChars));
    }
    if (const auto extensions = this->extensions()) {
        reply(extensions->request_note(*this, role, text.get<std::string>()));
        return;
    }
    try {
        engine_.request_note_adjustment(text.get<std::string>(), [reply](Json result) {
            reply(DashboardResponse::json(std::move(result)));
        });
    } catch (const std::invalid_argument& error) {
        throw HttpError(400, error.what());
    } catch (const EngineRefusal& error) {
        throw HttpError(409, error.what());
    }
}

void DashboardApi::end(const DashboardRequest& request, Reply reply) {
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require(token, true);
    const auto started = clock_.steady();
    engine_.end();
    const auto requested = request.received;
    const double elapsed = seconds(clock_.steady() - requested);
    if (elapsed > detail::kSlowEndWarnS) {
        // The wait split tells a saturated pool from an engine.end() that blocks (it should only
        // start its worker).
        const double queued = seconds(started - requested);
        logger.warning("Meeting end request took {:.2f}s ({:.2f}s waiting for a worker thread, "
                       "{:.2f}s in engine.end)",
                       elapsed, queued, elapsed - queued);
    }
    reply(DashboardResponse::json(Json{{"ok", true}}));
}

void DashboardApi::pause(const DashboardRequest& request, Reply reply) {
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require(token, true);
    engine_.pause();
    reply(DashboardResponse::json(Json{{"ok", true}}));
}

void DashboardApi::resume(const DashboardRequest& request, Reply reply) {
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require(token, true);
    engine_.resume();
    reply(DashboardResponse::json(Json{{"ok", true}}));
}

void DashboardApi::cloud(const DashboardRequest& request, Reply reply) {
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require(token, true);
    const Json body = json_body(request.http);
    // bool(body.get("enabled")): any truthy value enables, the string "false" too.
    const bool enabled = detail::truthy(detail::get(body, "enabled"));
    if (const auto extensions = this->extensions()) {
        reply(extensions->set_cloud(*this, enabled));
        return;
    }
    if (enabled) {
        if (!options_.request_cloud_consent) {
            throw HttpError(403, std::string(detail::kCloudConsentMissing));
        }
        const auto meeting_id = engine_.meeting_id();
        if (meeting_id.empty() || !is_active_meeting(meeting_id)) {
            throw HttpError(409, "AI insights can only be changed during a meeting.");
        }
        CancelSource permit;
        {
            const std::lock_guard lock(mutex_);
            if (cloud_prompt_ && !cloud_prompt_->cancelled()) {
                throw HttpError(409, "An AI insights request is awaiting desktop approval.");
            }
            cloud_prompt_ = permit;
        }
        try {
            options_.request_cloud_consent(
                meeting_id, permit.token(),
                [permit, &settings = settings_, reply = std::move(reply)](auto error) mutable {
                    if (permit.cancelled()) {
                        reply(
                            DashboardResponse::error(409, "The AI insights request was stopped."));
                        return;
                    }
                    permit.cancel();
                    if (!error) {
                        // _remember_cloud_choice: the toggle is the next meeting's default (the
                        // next meeting still asks for its own approval).
                        try {
                            settings.save_setting(keys::meeting_cloud_last_enabled.key(), true);
                        } catch (const std::exception& failure) {
                            logger.warning("Could not persist the AI insights toggle ({})",
                                           failure.what());
                        }
                    }
                    reply(error ? DashboardResponse::error(403, *error)
                                : DashboardResponse::json(Json{{"ok", true}, {"enabled", true}}));
                });
        } catch (...) {
            permit.cancel();
            throw;
        }
        return;
    }
    cancel_cloud_prompt();
    try {
        engine_.set_cloud_enabled(false);
    } catch (const EngineRefusal& refusal) {
        // OpenWhisper's archive raises here and the client gets a bare 500.
        throw HttpError(409, refusal.what());
    }
    try {
        // The toggle is the next meeting's default, as on the desktop.
        settings_.save_setting(keys::meeting_cloud_last_enabled.key(), false);
    } catch (const std::exception& error) {
        logger.warning("Could not persist the AI insights toggle ({})", error.what());
    }
    reply(DashboardResponse::json(Json{{"ok", true}, {"enabled", false}}));
}

void DashboardApi::cancel_cloud_prompt() {
    std::optional<CancelSource> prompt;
    {
        const std::lock_guard lock(mutex_);
        prompt = std::exchange(cloud_prompt_, std::nullopt);
    }
    if (prompt) {
        prompt->cancel();
    }
}

void DashboardApi::regenerate_tokens(const DashboardRequest& request, Reply reply) {
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require(token, true);
    cancel_cloud_prompt();
    const Json result = engine_.regenerate_tokens();
    Json body{{"ok", true}};
    if (result.is_object()) {
        for (const auto& [key, value] : result.items()) {
            body[key] = value;
        }
    }
    reply(DashboardResponse::json(body));
}

} // namespace fv::meeting_web
