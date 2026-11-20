#include "meeting_intel/recall.h"

#include <algorithm>
#include <exception>
#include <format>
#include <map>
#include <optional>
#include <utility>

#include "base/log.h"
#include "meeting_intel/detail/text.h"
#include "meeting_intel/intel_settings.h"
#include "meeting_intel/semantic_search.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::meeting_intel {

namespace {

constexpr log::Logger logger("meeting.recall");

Json result(bool ok, std::string text, Json hits = Json::array()) {
    Json out = Json::object();
    out["ok"] = ok;
    out["text"] = std::move(text);
    out["hits"] = std::move(hits);
    return out;
}

Json disabled(std::string_view text) {
    Json out = Json::object();
    out["ok"] = false;
    out["disabled"] = true;
    out["text"] = std::string(text);
    out["hits"] = Json::array();
    return out;
}

std::string opaque_ref(std::string_view meeting_id, std::int64_t index) {
    return "past:" + std::string(meeting_id) + ":" + std::to_string(index);
}

const Json& field(const Json& row, std::string_view key) {
    static const Json missing;
    if (!row.is_object()) {
        return missing;
    }
    const auto found = row.find(std::string(key));
    return found == row.end() ? missing : *found;
}

Json search_meetings(RecallDeps& deps, std::string_view query, std::string_view current_meeting_id,
                     std::int64_t limit, const CancelToken& cancel) {
    const std::string cleaned = py::strip(query);
    if (cleaned.empty()) {
        return result(false, "Provide a search query or a past meeting id.");
    }
    std::optional<std::string> exclude;
    if (!current_meeting_id.empty()) {
        exclude = std::string(current_meeting_id);
    }
    const auto judges = deps.judges;
    const auto settings = deps.settings;
    const Json found = search_history(
        *deps.history, settings, cleaned, true, exclude, limit,
        [&judges, &settings, &deps]() -> std::shared_ptr<llm::Judge> {
            if (deps.local_judge) {
                auto judge = deps.local_judge();
                return judge && judge->is_local() ? judge : std::shared_ptr<llm::Judge>{};
            }
            return judges ? judges->judge(settings()) : nullptr;
        },
        deps.clock != nullptr ? *deps.clock : system_clock(), cancel);
    std::vector<Json> rows;
    for (const auto& row : found.at("results")) {
        rows.push_back(row);
    }
    return detail::format_hits(rows, limit, current_meeting_id);
}

} // namespace

namespace detail {

std::string format_g(double value) {
    return std::format("{:g}", value);
}

double start_seconds(const Json& value) {
    return py::py_float(value).value_or(0.0);
}

std::string title_of(const Json& value) {
    std::string title = py::strip(sanitize(text_or_empty(value)));
    return title.empty() ? "Untitled meeting" : title;
}

std::string date_label(const Json& started_at) {
    const std::string raw = py::strip(text_or_empty(started_at));
    if (py::len(raw) >= 10 && py::slice(raw, 4, 5) == "-") {
        return py::slice(raw, 0, 10);
    }
    return raw.empty() ? "unknown date" : raw;
}

Json format_hits(const std::vector<Json>& rows, std::int64_t limit,
                 std::string_view current_meeting_id) {
    Json hits = Json::array();
    std::string text;
    std::size_t lines = 0;
    auto budget = static_cast<std::int64_t>(kRecallMaxTotalChars);
    std::map<std::string, std::int64_t, std::less<>> per_meeting;
    for (const auto& row : rows) {
        if (static_cast<std::int64_t>(hits.size()) >= limit || budget <= 0) {
            break;
        }
        const std::string meeting_id = py::strip(text_or_empty(field(row, "meeting_id")));
        if (meeting_id.empty() || meeting_id == current_meeting_id) {
            continue;
        }
        const Json& snippet_source =
            truthy(field(row, "snippet")) ? field(row, "snippet") : field(row, "text");
        const std::string snippet = clip(text_or_empty(snippet_source), kRecallMaxSnippetChars);
        if (snippet.empty()) {
            continue;
        }
        const std::int64_t index = ++per_meeting[meeting_id];
        const std::string ref = opaque_ref(meeting_id, index);
        const std::string title = title_of(field(row, "title"));
        const std::string date = date_label(field(row, "started_at"));
        const double start_s = start_seconds(field(row, "start_s"));
        const std::string line =
            ref + "  \"" + title + "\" (" + date + ") t=" + format_g(start_s) + "s — " + snippet;
        const auto length = static_cast<std::int64_t>(py::len(line));
        if (length > budget) {
            break;
        }
        text += (lines++ == 0 ? "" : "\n") + line;
        budget -= length + 1;
        Json hit = Json::object();
        hit["ref"] = ref;
        hit["meeting_id"] = meeting_id;
        hit["title"] = title;
        hit["started_at"] = date;
        hit["start_s"] = start_s;
        hit["snippet"] = snippet;
        hits.push_back(std::move(hit));
    }
    if (hits.empty()) {
        return result(true, "No past-meeting matches.");
    }
    return result(true, std::move(text), std::move(hits));
}

Json slice_meeting(HistorySource& history, std::string_view meeting_id,
                   std::string_view current_meeting_id, std::int64_t limit) {
    if (meeting_id == current_meeting_id) {
        return result(false, "Cannot recall the current meeting; it is already in the transcript.");
    }
    const std::optional<Json> meeting = history.get_meeting(meeting_id);
    if (!meeting || !truthy(*meeting)) {
        return result(false, "Unknown meeting.");
    }
    const std::vector<Json> segments =
        history.get_segments(meeting_id, std::min(limit, kRecallMaxSliceSegments));
    const std::string title = title_of(field(*meeting, "title"));
    const std::string date = date_label(field(*meeting, "started_at"));
    std::string text = "Meeting \"" + title + "\" (" + date + ")";
    std::size_t lines = 1;
    auto budget =
        static_cast<std::int64_t>(kRecallMaxTotalChars) - static_cast<std::int64_t>(py::len(text));
    Json hits = Json::array();
    std::int64_t index = 0;
    for (const auto& segment : segments) {
        ++index;
        if (index > kRecallMaxSliceSegments || budget <= 0) {
            break;
        }
        const std::string snippet =
            clip(text_or_empty(field(segment, "text")), kRecallMaxSnippetChars);
        if (snippet.empty()) {
            continue;
        }
        const std::string ref = opaque_ref(meeting_id, index);
        const double start_s = start_seconds(field(segment, "start_s"));
        const std::string line = ref + "  t=" + format_g(start_s) + "s  " + snippet;
        const auto length = static_cast<std::int64_t>(py::len(line));
        if (length > budget) {
            break;
        }
        text += "\n" + line;
        ++lines;
        budget -= length + 1;
        Json hit = Json::object();
        hit["ref"] = ref;
        hit["meeting_id"] = std::string(meeting_id);
        hit["title"] = title;
        hit["started_at"] = date;
        hit["start_s"] = start_s;
        hit["snippet"] = snippet;
        hits.push_back(std::move(hit));
    }
    if (lines == 1) {
        return result(true, "No transcript text in \"" + title + "\" (" + date + ").");
    }
    return result(true, std::move(text), std::move(hits));
}

} // namespace detail

bool past_recall_enabled(const std::function<Json()>& settings) {
    try {
        return resolve_meeting_past_recall_enabled(settings());
    } catch (const std::exception& error) {
        logger.warning("Could not read the past-meeting recall setting; recall stays off ({})",
                       detail::type_name(error));
        return false;
    }
}

Json search_past_meetings(RecallDeps& deps, std::string_view query,
                          std::string_view current_meeting_id, std::string_view meeting_id,
                          const Json& limit, const CancelToken& cancel) {
    if (!past_recall_enabled(deps.settings)) {
        return disabled("Past-meeting recall is disabled.");
    }
    if (deps.history == nullptr) {
        return disabled("Past-meeting recall is not available.");
    }
    const std::string current_id = py::strip(current_meeting_id);
    const std::string target_id = py::strip(meeting_id);
    const std::int64_t hit_limit =
        detail::clamp_limit(limit, kRecallDefaultHitLimit, kRecallMaxHitLimit);
    try {
        if (!target_id.empty()) {
            return detail::slice_meeting(*deps.history, target_id, current_id, hit_limit);
        }
        return search_meetings(deps, query, current_id, hit_limit, cancel);
    } catch (const std::exception& error) {
        logger.error_with(error, "Past-meeting recall failed");
        return result(false, "Past-meeting recall failed.");
    }
}

} // namespace fv::meeting_intel
