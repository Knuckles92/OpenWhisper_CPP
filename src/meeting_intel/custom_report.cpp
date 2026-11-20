// meeting/custom_report.py:1004-1116: claiming a report under the state store and writing it
// in the background.

#include "meeting_intel/custom_report.h"

#include <exception>
#include <optional>
#include <thread>
#include <utility>

#include "base/log.h"
#include "meeting_intel/detail/report.h"
#include "meeting_intel/detail/text.h"
#include "meeting_state/custom_report_ops.h"
#include "meeting_state/ids.h"
#include "pyport/text.h"

namespace fv::meeting_intel {

namespace {

constexpr log::Logger logger("meeting.custom_report");

const std::string kWorkerActor(meeting_state::WORKER_ACTOR);

Json finish_op(std::string_view report_id, std::string_view run_id) {
    return Json{{"op", "finish_custom_report"}, {"report_id", report_id}, {"run_id", run_id}};
}

void publish(meeting_state::MeetingStateStore& store, const Json& finish) {
    store.apply("system", kWorkerActor, Json::array({finish}));
}

} // namespace

void run_custom_report(const std::shared_ptr<meeting_state::MeetingStateStore>& store,
                       ReportDeps& deps, std::string_view report_id, std::string_view run_id,
                       std::string_view request, const ReportRoute& route, double timeout_s) {
    Json finish = finish_op(report_id, run_id);
    const auto stopped = [&] {
        return deps.cancel.cancelled() || (deps.destination_allowed && !deps.destination_allowed());
    };
    try {
        if (stopped()) {
            throw ReportUnavailable("Report writing was stopped. Ask for it again when ready.");
        }
        const std::string meeting_id = store->meeting_id();
        const std::optional<Json> row = deps.repository.get_meeting(meeting_id);
        const Json meeting = row && detail::truthy(*row) ? *row : Json::object();
        const std::vector<Json> segments =
            deps.repository.get_segments(meeting_id, -1.0, std::nullopt);
        const Json snapshot = store->snapshot();
        const Json result =
            deps.generator ? deps.generator(meeting, snapshot, segments, request, route, timeout_s)
                           : generate_report(meeting, snapshot, segments, request, route, deps,
                                             timeout_s, deps.cancel);
        if (stopped()) {
            throw ReportUnavailable("Report writing was stopped. Ask for it again when ready.");
        }
        if (!result.is_object()) {
            throw py::PyError("AttributeError", "the report result has no attribute 'get'");
        }
        const Json& sources = detail::member(result, "sources");
        finish["status"] = "ready";
        finish["markdown"] = result.contains("markdown") ? result.at("markdown") : Json("");
        finish["title"] = result.contains("title") ? result.at("title") : Json("");
        finish["sources"] = detail::truthy(sources) ? sources : Json::object();
        finish["message"] = "";
    } catch (const ReportUnavailable& error) {
        finish["status"] = "failed";
        finish["message"] =
            py::slice(error.what(), 0, static_cast<std::int64_t>(meeting_state::MAX_MESSAGE_CHARS));
    } catch (const std::exception& error) {
        // Only the type: the message can quote the request, the transcript or the report.
        logger.error("Custom report failed for report {} ({})", report_id,
                     detail::type_name(error));
        finish["status"] = "failed";
        finish["message"] = "The report could not be written. Your meeting is saved; retry.";
    } catch (...) {
        logger.error("Custom report failed for report {}", report_id);
        finish["status"] = "failed";
        finish["message"] = "The report could not be written. Your meeting is saved; retry.";
    }
    publish(*store, finish);
}

Json start_custom_report(std::shared_ptr<meeting_state::MeetingStateStore> store, ReportDeps& deps,
                         std::string_view request, const ReportRoute& route,
                         std::string_view actor_id, double timeout_s) {
    const std::string run_id = meeting_state::new_id("run");
    const std::optional<std::string> actor =
        actor_id.empty() ? std::nullopt : std::optional<std::string>(actor_id);
    const auto results = store->apply(
        "host", actor,
        Json::array(
            {Json{{"op", "request_custom_report"}, {"request", request}, {"run_id", run_id}}}));
    Json out = Json::object();
    if (results.empty() || !results[0].ok) {
        out["ok"] = false;
        out["report_id"] = nullptr;
        if (results.empty()) {
            out["error"] = "rejected";
        } else if (results[0].reason) {
            out["error"] = *results[0].reason;
        } else {
            out["error"] = nullptr;
        }
        return out;
    }
    const std::string report_id = results[0].target_id.value_or("");

    Task task = [store, deps, report_id, run_id, request = std::string(request), route,
                 timeout_s]() mutable {
        run_custom_report(store, deps, report_id, run_id, request, route, timeout_s);
    };
    try {
        if (deps.worker != nullptr) {
            deps.worker->post(std::move(task));
        } else {
            std::thread([task = std::move(task)] {
                detail::set_current_thread_name("fv-meeting-custom-report");
                // An exception leaving a std::thread ends the process; Python's thread only
                // printed it. run_custom_report catches the pass's own failures, so this is
                // the store's publish failing.
                try {
                    task();
                } catch (const std::exception& error) {
                    logger.error("The custom-report worker failed ({})", detail::type_name(error));
                } catch (...) {
                    logger.error("The custom-report worker failed");
                }
            }).detach();
        }
    } catch (const std::exception& error) {
        logger.error("Could not start the custom-report worker ({})", detail::type_name(error));
        Json finish = finish_op(report_id, run_id);
        finish["status"] = "failed";
        finish["message"] = "The report could not be started. Retry later.";
        publish(*store, finish);
        out["ok"] = false;
        out["report_id"] = report_id;
        out["error"] = "start_failed";
        return out;
    }
    out["ok"] = true;
    out["report_id"] = report_id;
    out["error"] = nullptr;
    return out;
}

} // namespace fv::meeting_intel
