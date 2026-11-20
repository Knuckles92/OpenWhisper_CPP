#include "meeting_state/custom_report_ops.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "meeting_state/detail/py_value.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::meeting_state {

namespace {

using detail::get;
using detail::truthy;

Json effect(const CustomReport& report, bool removed) {
    Json out = Json::object();
    out["entity"] = "custom_report";
    out["report"] = report.to_json();
    out["removed"] = removed;
    return out;
}

OpResult accepted(const Json& op, const CustomReport& report, bool removed = false) {
    OpResult out;
    out.ok = true;
    out.op = op;
    out.target_id = report.id;
    out.effect = effect(report, removed);
    return out;
}

// _clip: text cut to `limit` code points; anything else is "".
std::string clip(const Json& value, std::size_t limit) {
    if (!value.is_string()) {
        return "";
    }
    return py::slice(value.get_ref<const std::string&>(), 0, static_cast<std::int64_t>(limit));
}

// _ready_for_reports: a report reads the finished corpus, so it waits for the gate the insight
// review uses: the meeting is over and consolidation is not rewriting the cards underneath.
bool ready_for_reports(const MeetingState& state) {
    return state.status == "ended" && state.finalization.status != "pending" &&
           state.finalization.status != "running";
}

bool host_or_system(const OpContext& ctx) {
    return ctx.actor_type == "host" || ctx.actor_type == "system";
}

// state.find_custom_report(str(op.get("report_id") or "")).
CustomReport* find_report(MeetingState& state, const Json& op) {
    const Json& id = get(op, "report_id");
    return state.find_custom_report(truthy(id) ? detail::py_str(id) : std::string());
}

} // namespace

OpResult request_custom_report(MeetingState& state, Json& op, const OpContext& ctx) {
    if (!host_or_system(ctx)) {
        return reject(op, "host_only");
    }
    if (!ready_for_reports(state)) {
        return reject(op, "meeting_not_ready");
    }
    const Json& request = get(op, "request");
    if (!request.is_string() || py::strip(request.get_ref<const std::string&>()).empty()) {
        return reject(op, "invalid_request");
    }
    if (py::len(request.get_ref<const std::string&>()) > MAX_REQUEST_CHARS) {
        return reject(op, "request_too_long");
    }
    const Json& run_id = get(op, "run_id");
    if (!run_id.is_string() || run_id.get_ref<const std::string&>().empty()) {
        return reject(op, "invalid_run_id");
    }
    if (std::any_of(state.custom_reports.begin(), state.custom_reports.end(),
                    [](const CustomReport& report) { return report.status == "running"; })) {
        return reject(op, "report_running");
    }
    if (state.custom_reports.size() >= MAX_CUSTOM_REPORTS) {
        return reject(op, "report_limit_reached");
    }
    CustomReport report;
    report.id = ctx.new_id("rep");
    report.request = py::strip(request.get_ref<const std::string&>());
    report.status = "running";
    report.run_id = run_id.get<std::string>();
    report.message = "Reading the meeting and writing your report\xE2\x80\xA6";
    report.requested_by = ctx.actor_id;
    report.created_at = ctx.now_iso();
    report.updated_at = report.created_at;
    state.custom_reports.push_back(report);
    return accepted(op, report);
}

OpResult finish_custom_report(MeetingState& state, Json& op, const OpContext& ctx) {
    if (ctx.actor_type != "system" || ctx.actor_id != WORKER_ACTOR) {
        return reject(op, "system_only");
    }
    CustomReport* report = find_report(state, op);
    if (report == nullptr) {
        return reject(op, "unknown_report");
    }
    // A stale run cannot overwrite a report the host deleted and asked for again: the id may
    // repeat in a retry, the run id never does.
    if (report->status != "running" || !py::py_equal(Json(report->run_id), get(op, "run_id"))) {
        return reject(op, "stale_report");
    }
    const Json& status = get(op, "status");
    if (!status.is_string() ||
        std::find(CUSTOM_REPORT_STATUSES.begin(), CUSTOM_REPORT_STATUSES.end(),
                  status.get_ref<const std::string&>()) == CUSTOM_REPORT_STATUSES.end() ||
        status.get_ref<const std::string&>() == "running") {
        return reject(op, "invalid_report_status");
    }
    std::string markdown = clip(get(op, "markdown"), MAX_REPORT_CHARS);
    if (status.get_ref<const std::string&>() == "ready" && py::strip(markdown).empty()) {
        return reject(op, "empty_report");
    }
    report->status = status.get<std::string>();
    report->markdown = std::move(markdown);
    report->title = py::strip(clip(get(op, "title"), MAX_TITLE_CHARS));
    report->message = clip(get(op, "message"), MAX_MESSAGE_CHARS);
    const Json& sources = get(op, "sources");
    report->sources = sources.is_object() ? sources : Json::object();
    report->updated_at = ctx.now_iso();
    return accepted(op, *report);
}

OpResult remove_custom_report(MeetingState& state, Json& op, const OpContext& ctx) {
    if (!host_or_system(ctx)) {
        return reject(op, "host_only");
    }
    const CustomReport* report = find_report(state, op);
    if (report == nullptr) {
        return reject(op, "unknown_report");
    }
    const CustomReport removed = *report;
    std::erase_if(state.custom_reports,
                  [&](const CustomReport& entry) { return entry.id == removed.id; });
    return accepted(op, removed, true);
}

} // namespace fv::meeting_state
