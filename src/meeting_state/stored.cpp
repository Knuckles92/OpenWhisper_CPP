#include "meeting_state/stored.h"

#include <exception>
#include <utility>

#include "base/log.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/report_views.h"
#include "meeting_state/segment_ops.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::meeting_state {

namespace {

constexpr log::Logger logger("meeting.stored");

using detail::get;
using detail::has;
using detail::truthy;

// str(a or b or fallback).
std::string first_text(const Json& a, const Json& b, std::string_view fallback) {
    if (truthy(a)) {
        return detail::py_str(a);
    }
    if (truthy(b)) {
        return detail::py_str(b);
    }
    return std::string(fallback);
}

// The saved document as a past meeting is served after a restart.
Json historical_payload(const Json& meeting, const std::string& meeting_id, const Json& data) {
    Json payload = data.is_object() ? data : Json::object();
    payload["meeting_id"] = meeting_id;
    payload["title"] = first_text(get(meeting, "title"), get(payload, "title"), "");
    payload["status"] = first_text(get(meeting, "status"), get(payload, "status"), "ended");
    const bool cloud_enabled =
        has(meeting, "cloud_enabled") && truthy(get(meeting, "cloud_enabled"));
    payload["cloud_enabled"] = cloud_enabled;
    if (!payload.contains("seq")) {
        const Json& seq = get(meeting, "state_seq");
        payload["seq"] = truthy(seq) ? detail::py_int_or_throw(seq) : 0;
    }
    payload["finalization"] =
        FinalizationState::normalize_historical(get(payload, "finalization"), cloud_enabled,
                                                payload.at("status").get<std::string>())
            .to_json();
    const Json& review = get(payload, "insight_review");
    if (review.is_object() && get(review, "status") == "running") {
        Json stopped = review;
        stopped["status"] = "unavailable";
        stopped["message"] = "Review was interrupted. Retry when ready.";
        payload["insight_review"] = std::move(stopped);
    }
    const Json& reports = get(payload, "custom_reports");
    if (reports.is_array()) {
        // A report whose worker died with the process is not still being written; showing it
        // as running would also block every retry.
        Json kept = Json::array();
        for (const auto& report : reports) {
            if (report.is_object() && get(report, "status") == "running") {
                Json failed = report;
                failed["status"] = "failed";
                failed["message"] = "This report was interrupted. Ask for it again.";
                kept.push_back(std::move(failed));
            } else {
                kept.push_back(report);
            }
        }
        payload["custom_reports"] = std::move(kept);
    }
    return payload;
}

MeetingState fresh_state(const Json& meeting, const std::string& meeting_id, const Json& settings) {
    std::vector<std::string> report_views;
    try {
        report_views = resolve_meeting_report_views(settings);
    } catch (const std::exception& error) {
        logger.warning("Could not read meeting report views; using defaults: {}", error.what());
        report_views = {"ribbon", "brief", "signal"};
    }
    MeetingState state;
    state.meeting_id = meeting_id;
    const Json& title = get(meeting, "title");
    state.title = title.is_string() ? title.get<std::string>()
                                    : (title.is_null() ? std::string() : detail::py_str(title));
    // Carried over so the store's write-through cannot flip the recorded flag on a meeting that
    // never got a snapshot.
    state.cloud_enabled = truthy(get(meeting, "cloud_enabled"));
    state.report_views = std::move(report_views);
    return state;
}

// The repository a predicate reads. Without one, the predicate fails as Python's lambda over
// None does, rather than dereferencing nothing.
MeetingRepository& require(const std::shared_ptr<MeetingRepository>& repository,
                           std::string_view method) {
    if (!repository) {
        throw py::PyError("AttributeError",
                          "'NoneType' object has no attribute '" + std::string(method) + "'");
    }
    return *repository;
}

} // namespace

Json stored_state_dict(const Json& meeting) {
    return parse_state_json(get(meeting, "state_json")).value_or(Json::object());
}

std::optional<Json> stored_endpoint(const Json& meeting) {
    const Json& raw = get(meeting, "agent_endpoint_json");
    if (raw.is_object()) {
        return std::optional<Json>(std::in_place, raw);
    }
    if (raw.is_string() && !py::strip(raw.get_ref<const std::string&>()).empty()) {
        try {
            Json parsed = py::loads(raw.get_ref<const std::string&>());
            if (parsed.is_object()) {
                return std::optional<Json>(std::in_place, std::move(parsed));
            }
        } catch (const py::ValueError&) {
        }
    }
    return std::nullopt;
}

FinalizationState finalization_from_meeting_row(const Json& meeting) {
    const Json data = stored_state_dict(meeting);
    const Json& cloud =
        has(data, "cloud_enabled") ? get(data, "cloud_enabled") : get(meeting, "cloud_enabled");
    return FinalizationState::normalize_historical(
        get(data, "finalization"), truthy(cloud),
        first_text(get(meeting, "status"), Json(), "ended"));
}

Json compact_finalization_list_fields(const Json& meeting) {
    const std::string status = first_text(get(meeting, "status"), Json(), "ended");
    const FinalizationState fin = finalization_from_meeting_row(meeting);
    Json fields = Json::object();
    fields["finalization_status"] = fin.status;
    fields["finalization_deferred"] = fin.card_deferred;
    if (const auto pill = fin.history_pill(status)) {
        fields["insights_pill"] = pill->first;
        fields["insights_tone"] = pill->second;
    }
    return fields;
}

MeetingState load_state(const Json& meeting, bool historical, const Json& settings,
                        const Clock& clock) {
    return load_state(meeting, std::string_view(), historical, settings, clock);
}

MeetingState load_state(const Json& meeting, std::string_view meeting_id, bool historical,
                        const Json& settings, const Clock& clock) {
    const std::string id =
        !meeting_id.empty() ? std::string(meeting_id) : first_text(get(meeting, "id"), Json(), "");
    const Json& raw = get(meeting, "state_json");
    const std::optional<Json> data = parse_state_json(raw);
    if (!data && truthy(raw)) {
        logger.warning("Unreadable state_json for meeting {}; starting from a fresh state", id);
    }
    if (historical) {
        try {
            return MeetingState::from_json(
                historical_payload(meeting, id, data.value_or(Json::object())), clock);
        } catch (const std::exception& error) {
            logger.error_with(error, "Corrupt state_json for meeting " + id +
                                         "; starting from a fresh state");
            return MeetingState::from_json(historical_payload(meeting, id, Json::object()), clock);
        }
    }
    if (data) {
        try {
            return MeetingState::from_json(*data, clock);
        } catch (const std::exception& error) {
            logger.error_with(error, "Corrupt state_json for meeting " + id +
                                         "; starting from a fresh state");
        }
    }
    return fresh_state(meeting, id, settings);
}

std::shared_ptr<MeetingStateStore> open_store(std::shared_ptr<MeetingRepository> repository,
                                              std::string_view meeting_id, const Json& meeting,
                                              bool historical, const Json& settings,
                                              const Clock& clock, IdSource& ids) {
    const std::string id(meeting_id);
    SegmentPredicates predicates;
    predicates.segment_exists = [repository, id](std::string_view segment_id) {
        return require(repository, "segment_exists").segment_exists(id, segment_id);
    };
    predicates.segment_pinned = [repository, id](std::string_view segment_id) {
        const std::optional<Json> row =
            require(repository, "get_segment").get_segment(id, segment_id);
        return row && truthy(get(*row, "speaker_pinned"));
    };
    predicates.segment_lookup = repository_segment_lookup(repository, id);
    return std::make_shared<MeetingStateStore>(load_state(meeting, id, historical, settings, clock),
                                               repository, make_segment_handler(repository, id),
                                               clock, ids, std::move(predicates));
}

std::shared_ptr<MeetingStateStore> open_store(std::shared_ptr<MeetingRepository> repository,
                                              const Json& meeting, bool historical,
                                              const Json& settings, const Clock& clock) {
    const std::string id = first_text(get(meeting, "id"), Json(), "");
    return open_store(std::move(repository), id, meeting, historical, settings, clock);
}

} // namespace fv::meeting_state
