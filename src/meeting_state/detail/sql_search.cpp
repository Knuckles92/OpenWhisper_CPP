// SqlMeetingRepository: the Past Meetings page and transcript search
// (repository.py list_past_meeting_summaries, _content_aggregates, _search_filter,
// _origin_filter, get_meeting_content_summary, list_past_meeting_times, search_transcripts).

#include <algorithm>
#include <exception>
#include <map>
#include <string>
#include <utility>

#include "base/log.h"
#include "db/database.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/detail/sql_internal.h"
#include "meeting_state/sql_repository.h"
#include "pyport/text.h"

namespace fv::meeting_state {

namespace {

constexpr log::Logger logger("meeting.persist.repository");

// Longest preview_text before it is clipped, in code points.
constexpr std::size_t kPreviewLimit = 100;

struct Aggregates {
    std::map<std::string, std::pair<std::int64_t, bool>, std::less<>> audio;
    std::map<std::string, std::int64_t, std::less<>> transcript_count;
    std::map<std::string, std::string, std::less<>> preview;
};

// Per meeting: (audio chunk count, has loopback), transcript count and first line, in three
// aggregate reads rather than one query per meeting.
Aggregates content_aggregates(sql::Connection& connection, const std::vector<std::string>& ids) {
    std::vector<sql::Value> params(ids.begin(), ids.end());
    const std::string in = "(" + detail::placeholders(ids.size()) + ")";
    Aggregates out;
    {
        sql::Statement statement = connection.prepare(
            "SELECT meeting_audio_chunks.meeting_id, count(meeting_audio_chunks.id), "
            "max(CASE WHEN (meeting_audio_chunks.channel = 'loopback') THEN 1 ELSE 0 END) FROM "
            "meeting_audio_chunks WHERE meeting_audio_chunks.meeting_id IN " +
            in +
            " AND meeting_audio_chunks.duration_s > 0.0 GROUP BY "
            "meeting_audio_chunks.meeting_id");
        detail::bind_all(statement, params);
        while (statement.step()) {
            out.audio[detail::text_value(statement.column(0))] = {
                detail::int_value(statement.column(1)),
                detail::int_value(statement.column(2)) != 0};
        }
    }
    {
        sql::Statement statement = connection.prepare(
            "SELECT meeting_segments.meeting_id, count(meeting_segments.id) FROM meeting_segments "
            "WHERE meeting_segments.meeting_id IN " +
            in + " AND trim(meeting_segments.text) != '' GROUP BY meeting_segments.meeting_id");
        detail::bind_all(statement, params);
        while (statement.step()) {
            out.transcript_count[detail::text_value(statement.column(0))] =
                detail::int_value(statement.column(1));
        }
    }
    {
        sql::Statement statement = connection.prepare(
            "SELECT anon_1.meeting_id, anon_1.text FROM (SELECT meeting_segments.meeting_id AS "
            "meeting_id, meeting_segments.text AS text, row_number() OVER (PARTITION BY "
            "meeting_segments.meeting_id ORDER BY meeting_segments.start_s, meeting_segments.id) "
            "AS rank FROM meeting_segments WHERE meeting_segments.meeting_id IN " +
            in + " AND trim(meeting_segments.text) != '') AS anon_1 WHERE anon_1.rank = 1");
        detail::bind_all(statement, params);
        while (statement.step()) {
            const Json text = detail::from_value(statement.column(1));
            out.preview[detail::text_value(statement.column(0))] =
                detail::truthy(text) ? detail::py_str(text) : std::string();
        }
    }
    return out;
}

} // namespace

namespace detail {

Json content_summary(std::int64_t audio_count, bool has_loopback, std::int64_t segment_count,
                     std::string preview) {
    if (py::len(preview) > kPreviewLimit) {
        preview = py::rstrip(py::slice(preview, 0, kPreviewLimit)) + "…";
    }
    Json summary = Json::object();
    summary["has_audio"] = audio_count > 0;
    summary["has_loopback_audio"] = has_loopback;
    summary["has_transcript"] = segment_count > 0;
    summary["is_empty"] = audio_count == 0 && segment_count == 0;
    summary["audio_chunks"] = audio_count;
    summary["transcript_segments"] = segment_count;
    summary["can_rerun_speakers"] = has_loopback;
    summary["preview_text"] = preview;
    return summary;
}

std::string search_like_pattern(std::string_view needle) {
    std::string escaped;
    for (const char c : needle) {
        if (c == '\\' || c == '%' || c == '_') {
            escaped += '\\';
        }
        escaped += c;
    }
    return "%" + escaped + "%";
}

} // namespace detail

std::vector<Json> SqlMeetingRepository::list_past_meeting_summaries(const PastMeetingQuery& query) {
    const std::int64_t page_limit = std::clamp<std::int64_t>(query.limit, 1, 501);
    const std::string needle = py::strip(query.query);
    std::string sql =
        "SELECT " + std::string(detail::kSessionColumns) + " FROM meeting_sessions WHERE 1 = 1";
    std::vector<sql::Value> params;
    if (!query.include_running) {
        sql += " AND (meeting_sessions.status NOT IN ('active', 'paused', 'ending'))";
    }
    if (query.meeting_id) {
        sql += " AND meeting_sessions.id = ?";
        params.emplace_back(*query.meeting_id);
    }
    if (!query.started_after.empty()) {
        sql += " AND julianday(meeting_sessions.started_at) >= julianday(?)";
        params.emplace_back(query.started_after);
    }
    if (query.cursor_started_at && query.cursor_id) {
        sql += " AND (meeting_sessions.started_at < ? OR meeting_sessions.started_at = ? AND "
               "meeting_sessions.id < ?)";
        params.emplace_back(*query.cursor_started_at);
        params.emplace_back(*query.cursor_started_at);
        params.emplace_back(*query.cursor_id);
    }
    if (!query.started_from.empty()) {
        sql += " AND meeting_sessions.started_at >= ?";
        params.emplace_back(query.started_from);
    }
    if (!query.started_before.empty()) {
        sql += " AND meeting_sessions.started_at < ?";
        params.emplace_back(query.started_before);
    }
    switch (query.origin) {
    case PastMeetingQuery::Origin::Local:
        sql += " AND meeting_sessions.origin_device_id IS NULL";
        break;
    case PastMeetingQuery::Origin::Device:
        sql += " AND meeting_sessions.origin_device_id = ?";
        params.emplace_back(query.origin_device_id);
        break;
    case PastMeetingQuery::Origin::Any:
        break;
    }
    if (!needle.empty()) {
        // Matched literally against metadata, the saved state and the transcript.
        const std::string pattern = detail::search_like_pattern(needle);
        sql += " AND (lower(meeting_sessions.title) LIKE lower(?) ESCAPE '\\' OR "
               "lower(meeting_sessions.started_at) LIKE lower(?) ESCAPE '\\' OR "
               "lower(meeting_sessions.status) LIKE lower(?) ESCAPE '\\' OR "
               "lower(meeting_sessions.state_json) LIKE lower(?) ESCAPE '\\' OR "
               "meeting_sessions.id IN (SELECT DISTINCT meeting_segments.meeting_id FROM "
               "meeting_segments WHERE lower(meeting_segments.text) LIKE lower(?) ESCAPE '\\'))";
        for (int index = 0; index < 5; ++index) {
            params.emplace_back(pattern);
        }
    }
    sql += " ORDER BY meeting_sessions.started_at DESC, meeting_sessions.id DESC LIMIT ?";
    params.emplace_back(page_limit);

    int queries = 0;
    std::vector<Json> result = database_.read([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        std::vector<Json> rows;
        sql::Statement statement = connection.prepare(sql);
        detail::bind_all(statement, params);
        ++queries;
        while (statement.step()) {
            rows.push_back(detail::session_dict(statement));
        }
        if (rows.empty()) {
            return rows;
        }
        std::vector<std::string> ids;
        for (const auto& row : rows) {
            ids.push_back(detail::py_str(row.at("id")));
        }
        const Aggregates aggregates = content_aggregates(connection, ids);
        queries += 3;
        for (auto& row : rows) {
            const std::string id = detail::py_str(row.at("id"));
            const auto audio = aggregates.audio.find(id);
            const auto [audio_count, has_loopback] =
                audio != aggregates.audio.end() ? audio->second : std::pair<std::int64_t, bool>{};
            const auto count = aggregates.transcript_count.find(id);
            const std::int64_t segment_count =
                count != aggregates.transcript_count.end() ? count->second : 0;
            const auto preview = aggregates.preview.find(id);
            row["content_summary"] = detail::content_summary(
                audio_count, has_loopback, segment_count,
                py::strip(preview != aggregates.preview.end() ? preview->second : ""));
        }
        return rows;
    });
    std::lock_guard lock(mutex_);
    last_summary_queries_ = queries;
    return result;
}

Json SqlMeetingRepository::get_meeting_content_summary(std::string_view meeting_id) {
    PastMeetingQuery query;
    query.limit = 1;
    query.include_running = true;
    query.meeting_id = std::string(meeting_id);
    const std::vector<Json> rows = list_past_meeting_summaries(query);
    if (!rows.empty()) {
        return rows.front().at("content_summary");
    }
    return detail::content_summary(0, false, 0, "");
}

std::vector<PastMeetingTime> SqlMeetingRepository::list_past_meeting_times() {
    return database_.read([&](sql::Connection& connection) {
        sql::Statement statement = connection.prepare(
            "SELECT meeting_sessions.id, meeting_sessions.started_at, meeting_sessions.ended_at, "
            "meeting_sessions.paused_total_s, meeting_sessions.origin_device_name FROM "
            "meeting_sessions WHERE (meeting_sessions.status NOT IN ('active', 'paused', "
            "'ending'))");
        std::vector<PastMeetingTime> rows;
        while (statement.step()) {
            PastMeetingTime row;
            row.id = detail::text_value(statement.column(0));
            row.started_at = detail::text_value(statement.column(1));
            const sql::Value ended = statement.column(2);
            if (std::holds_alternative<std::string>(ended)) {
                row.ended_at = std::get<std::string>(ended);
            }
            row.paused_total_s = detail::real_value(statement.column(3));
            const sql::Value origin = statement.column(4);
            if (std::holds_alternative<std::string>(origin)) {
                row.origin_device_name = std::get<std::string>(origin);
            }
            rows.push_back(std::move(row));
        }
        return rows;
    });
}

std::vector<Json>
SqlMeetingRepository::search_transcripts(std::string_view query,
                                         const std::optional<std::string>& exclude_meeting_id,
                                         std::int64_t limit) {
    const std::string stripped = py::strip(query);
    if (stripped.empty()) {
        return {};
    }
    const std::int64_t row_limit = std::clamp<std::int64_t>(limit, 1, 200);
    // Every term quoted, so FTS5 operators in user input cannot change the match expression.
    std::string match;
    for (const auto& term : py::split(stripped)) {
        std::string quoted;
        for (const char c : term) {
            quoted += c == '"' ? std::string("\"\"") : std::string(1, c);
        }
        match += (match.empty() ? "\"" : " \"") + quoted + "\"";
    }
    const std::string exclude = py::strip(exclude_meeting_id.value_or(""));
    std::string sql =
        "SELECT ms.id AS segment_id, ms.meeting_id, ms.start_s, ms.end_s, ms.text, s.title, "
        "s.started_at, snippet(meeting_segments_fts, 0, '[', ']', ' … ', 12) AS snippet "
        "FROM meeting_segments_fts JOIN meeting_segments ms ON ms.rowid = "
        "meeting_segments_fts.rowid JOIN meeting_sessions s ON s.id = ms.meeting_id WHERE "
        "meeting_segments_fts MATCH :q";
    if (!exclude.empty()) {
        sql += " AND s.id != :exclude";
    }
    sql += " ORDER BY s.started_at DESC, ms.start_s LIMIT :limit";
    try {
        return database_.read([&](sql::Connection& connection) {
            sql::Statement statement = connection.prepare(sql);
            statement.bind(":q", match);
            statement.bind(":limit", row_limit);
            if (!exclude.empty()) {
                statement.bind(":exclude", exclude);
            }
            static constexpr std::string_view keys[] = {"segment_id", "meeting_id", "start_s",
                                                        "end_s",      "text",       "title",
                                                        "started_at", "snippet"};
            std::vector<Json> rows;
            while (statement.step()) {
                Json row = Json::object();
                for (int index = 0; index < 8; ++index) {
                    row[std::string(keys[index])] = detail::from_value(statement.column(index));
                }
                rows.push_back(std::move(row));
            }
            return rows;
        });
    } catch (const std::exception& error) {
        logger.error_with(error, "Meeting transcript search failed");
        return {};
    }
}

} // namespace fv::meeting_state
