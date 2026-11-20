// SqlMeetingRepository: state write-through, its mirrors and the audit trail
// (repository.py on_ops_applied, persist_state, _mirror_*, get_event .. event_is_undone).

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "db/database.h"
#include "history/titles.h"
#include "meeting_state/corrections.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/detail/sql_internal.h"
#include "meeting_state/sql_repository.h"
#include "meeting_state/time_utils.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::meeting_state {

namespace {

using detail::at;
using detail::get;
using detail::has;
using detail::truthy;

std::string dumps_unicode(const Json& value) {
    return py::dumps(value, {.ensure_ascii = false});
}

void exec_bound(sql::Connection& connection, std::string_view sql,
                const std::vector<sql::Value>& params) {
    sql::Statement statement = connection.prepare(sql);
    detail::bind_all(statement, params);
    statement.step();
}

// json.dumps(value or fallback), with or without ASCII escapes.
std::string dumps_or(const Json& value, const Json& fallback, bool ensure_ascii) {
    return py::dumps(truthy(value) ? value : fallback, {.ensure_ascii = ensure_ascii});
}

void mirror_item(sql::Connection& connection, std::string_view meeting_id, const Json& item) {
    exec_bound(
        connection,
        "INSERT INTO meeting_state_items (id, meeting_id, card, text, data_json, status, "
        "author_type, author_id, pinned, revision, evidence_json, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (id) DO UPDATE SET "
        "meeting_id = excluded.meeting_id, card = excluded.card, text = excluded.text, "
        "data_json = excluded.data_json, status = excluded.status, author_type = "
        "excluded.author_type, author_id = excluded.author_id, pinned = excluded.pinned, "
        "revision = excluded.revision, evidence_json = excluded.evidence_json, "
        "created_at = excluded.created_at, updated_at = excluded.updated_at",
        {detail::to_value(at(item, "id")), std::string(meeting_id),
         detail::to_value(at(item, "card")), detail::to_value(at(item, "text")),
         dumps_or(get(item, "data"), Json::object(), false), detail::to_value(at(item, "status")),
         detail::to_value(at(item, "author_type")), detail::to_value(get(item, "author_id")),
         std::int64_t{truthy(at(item, "pinned")) ? 1 : 0}, detail::to_value(at(item, "revision")),
         dumps_or(get(item, "evidence"), Json::array(), true),
         detail::to_value(at(item, "created_at")), detail::to_value(at(item, "updated_at"))});
}

void mirror_question(sql::Connection& connection, std::string_view meeting_id,
                     const Json& question) {
    exec_bound(connection,
               "INSERT INTO meeting_questions (id, meeting_id, text, status, answer_text, "
               "answer_source, answer_confidence, suggested_answer_text, suggested_confidence, "
               "thread_json, evidence_json, asked_at, resolved_at, resolved_by) VALUES (?, ?, ?, "
               "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (id) DO UPDATE SET meeting_id = "
               "excluded.meeting_id, text = excluded.text, status = excluded.status, "
               "answer_text = excluded.answer_text, answer_source = excluded.answer_source, "
               "answer_confidence = excluded.answer_confidence, suggested_answer_text = "
               "excluded.suggested_answer_text, suggested_confidence = "
               "excluded.suggested_confidence, thread_json = excluded.thread_json, "
               "evidence_json = excluded.evidence_json, asked_at = excluded.asked_at, "
               "resolved_at = excluded.resolved_at, resolved_by = excluded.resolved_by",
               {detail::to_value(at(question, "id")), std::string(meeting_id),
                detail::to_value(at(question, "text")), detail::to_value(at(question, "status")),
                detail::to_value(get(question, "answer")),
                detail::to_value(get(question, "answer_source")),
                detail::to_value(get(question, "confidence")),
                detail::to_value(get(question, "suggested_answer")),
                detail::to_value(get(question, "suggested_confidence")),
                dumps_or(get(question, "thread"), Json::array(), false),
                dumps_or(get(question, "evidence"), Json::array(), true),
                detail::to_value(at(question, "asked_at")),
                detail::to_value(get(question, "resolved_at")),
                detail::to_value(get(question, "resolved_by"))});
}

void mirror_participant(sql::Connection& connection, std::string_view meeting_id,
                        const Json& participant) {
    exec_bound(connection,
               "INSERT INTO meeting_participants (id, meeting_id, display_name, kind, "
               "name_source, is_provisional, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, "
               "?) ON CONFLICT (id) DO UPDATE SET meeting_id = excluded.meeting_id, "
               "display_name = excluded.display_name, kind = excluded.kind, name_source = "
               "excluded.name_source, is_provisional = excluded.is_provisional, created_at = "
               "excluded.created_at, updated_at = excluded.updated_at",
               {detail::to_value(at(participant, "id")), std::string(meeting_id),
                detail::to_value(at(participant, "display_name")),
                detail::to_value(at(participant, "kind")),
                detail::to_value(at(participant, "name_source")),
                std::int64_t{truthy(at(participant, "is_provisional")) ? 1 : 0},
                detail::to_value(at(participant, "created_at")),
                detail::to_value(at(participant, "updated_at"))});
}

// _meeting_segment_row: the segment must belong to the meeting.
void require_meeting_segment(sql::Connection& connection, std::string_view meeting_id,
                             const Json& segment_id) {
    sql::Statement statement = connection.prepare(
        "SELECT meeting_segments.id FROM meeting_segments WHERE meeting_segments.meeting_id = ? "
        "AND meeting_segments.id = ?");
    detail::bind_all(statement, {std::string(meeting_id), detail::to_value(segment_id)});
    if (!statement.step()) {
        throw py::ValueError("segment does not belong to meeting");
    }
}

void mirror_segment_speaker(sql::Connection& connection, std::string_view meeting_id,
                            const Json& effect) {
    const Json& segment_id = at(effect, "segment_id");
    require_meeting_segment(connection, meeting_id, segment_id);
    const Json& participant_id = get(effect, "participant_id");
    if (!participant_id.is_null()) {
        sql::Statement statement = connection.prepare(
            "SELECT meeting_participants.id FROM meeting_participants WHERE "
            "meeting_participants.meeting_id = ? AND meeting_participants.id = ? LIMIT 1");
        detail::bind_all(statement, {std::string(meeting_id), detail::to_value(participant_id)});
        if (!statement.step()) {
            throw py::ValueError("participant does not belong to meeting");
        }
    }
    exec_bound(connection,
               "UPDATE meeting_segments SET speaker_participant_id = ?, speaker_source = ?, "
               "speaker_pinned = ? WHERE meeting_segments.id = ?",
               {detail::to_value(participant_id),
                has(effect, "source") ? detail::to_value(get(effect, "source"))
                                      : sql::Value(std::string("human")),
                std::int64_t{truthy(get(effect, "pinned")) ? 1 : 0}, detail::to_value(segment_id)});
}

void mirror_segment_text(sql::Connection& connection, std::string_view meeting_id, Json& effect,
                         detail::SegmentReader& reader) {
    const Json segment_id = at(effect, "segment_id");
    require_meeting_segment(connection, meeting_id, segment_id);
    const Json& raw = get(effect, "text");
    const std::string text = truthy(raw) ? py::strip(detail::py_str(raw)) : std::string();
    if (text.empty()) {
        throw py::ValueError("segment text must be non-empty");
    }
    exec_bound(connection, "UPDATE meeting_segments SET text = ? WHERE meeting_segments.id = ?",
               {text, detail::to_value(segment_id)});
    // The broadcast payload stays aligned with the persisted row.
    if (auto row = reader.by_id(detail::py_str(segment_id))) {
        effect["segment"] = std::move(*row);
    }
}

void mirror_effect(sql::Connection& connection, std::string_view meeting_id, Json& effect,
                   detail::SegmentReader& reader) {
    if (!effect.is_object()) {
        return;
    }
    const Json& entity = get(effect, "entity");
    if (!entity.is_string()) {
        return;
    }
    const std::string& kind = entity.get_ref<const std::string&>();
    if (kind == "review") {
        const Json& items = get(effect, "items");
        if (items.is_array()) {
            for (const auto& item : items) {
                mirror_item(connection, meeting_id, item);
            }
        }
    } else if (kind == "item") {
        mirror_item(connection, meeting_id, at(effect, "item"));
    } else if (kind == "question") {
        mirror_question(connection, meeting_id, at(effect, "question"));
    } else if (kind == "participant") {
        mirror_participant(connection, meeting_id, at(effect, "participant"));
    } else if (kind == "segment_speaker") {
        mirror_segment_speaker(connection, meeting_id, effect);
    } else if (kind == "segment_text") {
        mirror_segment_text(connection, meeting_id, effect, reader);
    }
}

bool is(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

// Validates one-occurrence corrections against the raw rows inside the write transaction:
// rules come from the batch's final state (after any meeting-wide correction in it), before
// scoped replacements are projected onto the transcript.
void revalidate_scoped_corrections(sql::Connection& connection, std::string_view meeting_id,
                                   const Json& state, const std::vector<OpResult>& results) {
    std::vector<const OpResult*> scoped_adds;
    for (const auto& result : results) {
        if (is(get(result.op, "op"), "add_item") && is(get(result.op, "card"), "user_notes") &&
            is(get(get(result.op, "data"), "kind"), "occurrence_correction")) {
            scoped_adds.push_back(&result);
        }
    }
    if (scoped_adds.empty()) {
        return;
    }
    const TermRules global_rules = term_rules(state);
    std::map<std::string, std::string, std::less<>> revised_raw;
    for (const auto& result : results) {
        if (truthy(result.effect) && is(get(result.effect, "entity"), "segment_text")) {
            revised_raw[detail::py_str(at(result.effect, "segment_id"))] =
                detail::py_str(at(result.effect, "text"));
        }
    }
    for (const OpResult* result : scoped_adds) {
        const Json& evidence = at(result->op, "evidence");
        if (!evidence.is_array() || evidence.empty()) {
            throw py::IndexError("list index out of range");
        }
        const std::string segment_id = detail::py_str(evidence.at(0));
        std::optional<std::string> raw;
        if (const auto revised = revised_raw.find(segment_id); revised != revised_raw.end()) {
            raw = revised->second;
        } else {
            sql::Statement statement =
                connection.prepare("SELECT meeting_segments.text FROM meeting_segments WHERE "
                                   "meeting_segments.meeting_id = ? AND meeting_segments.id = ?");
            detail::bind_all(statement, {std::string(meeting_id), segment_id});
            if (statement.step()) {
                raw = detail::text_value(statement.column(0));
            }
        }
        const Json& expected = at(at(result->op, "data"), "base_fingerprint");
        if (!raw || !is(expected, segment_fingerprint(correct_text(*raw, global_rules)))) {
            throw StaleCorrectionSelection(segment_id);
        }
    }
}

struct SessionState {
    Json title;
    Json state_seq;
    Json status;
    Json cloud_enabled;
};

std::optional<SessionState> session_state(sql::Connection& connection,
                                          std::string_view meeting_id) {
    sql::Statement statement = connection.prepare(
        "SELECT meeting_sessions.title, meeting_sessions.state_seq, meeting_sessions.status, "
        "meeting_sessions.cloud_enabled FROM meeting_sessions WHERE meeting_sessions.id = ?");
    statement.bind(1, std::string(meeting_id));
    if (!statement.step()) {
        return std::nullopt;
    }
    return SessionState{
        detail::from_value(statement.column(0)), detail::from_value(statement.column(1)),
        detail::from_value(statement.column(2)), detail::bool_from_value(statement.column(3))};
}

Json event_row(const sql::Statement& statement, bool with_inverse) {
    Json event = Json::object();
    event["seq"] = detail::from_value(statement.column(0));
    event["ts"] = detail::from_value(statement.column(1));
    event["actor_type"] = detail::from_value(statement.column(2));
    event["actor_id"] = detail::from_value(statement.column(3));
    event["action"] = detail::from_value(statement.column(4));
    event["target_id"] = detail::from_value(statement.column(5));
    event["payload"] = py::loads(detail::text_value(statement.column(6)));
    if (with_inverse) {
        const std::string inverse = detail::text_value(statement.column(7));
        event["inverse"] = inverse.empty() ? Json() : py::loads(inverse);
    }
    return event;
}

constexpr std::string_view kEventColumns =
    "meeting_events.seq, meeting_events.ts, meeting_events.actor_type, meeting_events.actor_id, "
    "meeting_events.action, meeting_events.target_id, meeting_events.payload_json, "
    "meeting_events.inverse_json";

} // namespace

Json SqlMeetingRepository::on_ops_applied(std::string_view meeting_id, const Json& state,
                                          std::vector<OpResult>& results,
                                          std::string_view actor_type,
                                          const std::optional<std::string>& actor_id) {
    const std::string ts = utc_now_iso(clock_);
    Json snapshot = state;
    database_.write([&](sql::Connection& connection) {
        // A dashboard snapshot may predate an MCP or another dashboard's rename: the read, merge
        // and write are one immediate transaction, and only an explicit set_title retitles.
        sql::Transaction transaction = connection.begin(sql::TxMode::Immediate);
        revalidate_scoped_corrections(connection, meeting_id, snapshot, results);
        detail::SegmentReader reader(connection);
        for (auto& result : results) {
            const Json& undo_seq = get(result.op, "_undo_event_seq");
            Json action = has(result.op, "op") ? get(result.op, "op") : Json("unknown");
            if (actor_type == "system" && undo_seq.is_number_integer()) {
                action = "undo:" + std::to_string(undo_seq.get<std::int64_t>());
            }
            exec_bound(
                connection,
                "INSERT INTO meeting_events (meeting_id, seq, ts, actor_type, actor_id, "
                "action, target_id, payload_json, inverse_json) VALUES (?, ?, ?, ?, ?, ?, "
                "?, ?, ?)",
                {std::string(meeting_id), result.seq.value_or(0), ts, std::string(actor_type),
                 actor_id ? sql::Value(*actor_id) : sql::Value(nullptr), detail::to_value(action),
                 result.target_id ? sql::Value(*result.target_id) : sql::Value(nullptr),
                 dumps_unicode(result.op),
                 truthy(result.inverse) ? sql::Value(dumps_unicode(result.inverse))
                                        : sql::Value(nullptr)});
            mirror_effect(connection, meeting_id, result.effect, reader);
        }
        const std::optional<SessionState> row = session_state(connection, meeting_id);
        if (row) {
            Json title = row->title;
            for (const auto& result : results) {
                if (result.ok && is(get(result.op, "op"), "set_title")) {
                    title = normalize_title(get(result.op, "text"));
                }
            }
            snapshot["title"] = truthy(title) ? title : Json("");
            const std::int64_t seq =
                has(snapshot, "seq") ? detail::py_int_or_throw(get(snapshot, "seq")) : 0;
            const bool cloud = has(snapshot, "cloud_enabled")
                                   ? truthy(get(snapshot, "cloud_enabled"))
                                   : truthy(row->cloud_enabled);
            exec_bound(connection,
                       "UPDATE meeting_sessions SET title = ?, state_json = ?, state_seq = ?, "
                       "cloud_enabled = ? WHERE meeting_sessions.id = ?",
                       {detail::to_value(title), dumps_unicode(snapshot), seq,
                        std::int64_t{cloud ? 1 : 0}, std::string(meeting_id)});
        }
        transaction.commit();
    });
    return snapshot;
}

Json SqlMeetingRepository::persist_state(std::string_view meeting_id, const Json& state) {
    Json snapshot = state;
    database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin(sql::TxMode::Immediate);
        const std::optional<SessionState> row = session_state(connection, meeting_id);
        if (!row) {
            throw py::ValueError("unknown meeting '" + std::string(meeting_id) + "'");
        }
        snapshot["title"] = truthy(row->title) ? row->title : Json("");
        const std::int64_t seq =
            has(snapshot, "seq")
                ? detail::py_int_or_throw(get(snapshot, "seq"))
                : (truthy(row->state_seq) ? detail::py_int_or_throw(row->state_seq) : 0);
        const Json status = has(snapshot, "status") ? get(snapshot, "status") : row->status;
        const bool cloud = has(snapshot, "cloud_enabled") ? truthy(get(snapshot, "cloud_enabled"))
                                                          : truthy(row->cloud_enabled);
        exec_bound(connection,
                   "UPDATE meeting_sessions SET state_json = ?, state_seq = ?, status = ?, "
                   "cloud_enabled = ? WHERE meeting_sessions.id = ?",
                   {dumps_unicode(snapshot), seq, detail::to_value(status),
                    std::int64_t{cloud ? 1 : 0}, std::string(meeting_id)});
        transaction.commit();
    });
    return snapshot;
}

std::optional<Json> SqlMeetingRepository::get_event(std::string_view meeting_id, std::int64_t seq) {
    return database_.read([&](sql::Connection& connection) -> std::optional<Json> {
        sql::Statement statement =
            connection.prepare("SELECT " + std::string(kEventColumns) +
                               " FROM meeting_events WHERE meeting_events.meeting_id = ? AND "
                               "meeting_events.seq = ? LIMIT 1");
        detail::bind_all(statement, {std::string(meeting_id), seq});
        if (!statement.step()) {
            return std::nullopt;
        }
        return event_row(statement, true);
    });
}

bool SqlMeetingRepository::event_is_undone(std::string_view meeting_id, std::int64_t seq) {
    return database_.read([&](sql::Connection& connection) {
        sql::Statement statement = connection.prepare(
            "SELECT meeting_events.seq FROM meeting_events WHERE meeting_events.meeting_id = ? "
            "AND meeting_events.action = ? LIMIT 1");
        detail::bind_all(statement, {std::string(meeting_id), "undo:" + std::to_string(seq)});
        return statement.step();
    });
}

std::vector<Json> SqlMeetingRepository::list_events(std::string_view meeting_id,
                                                    std::optional<std::int64_t> before_seq,
                                                    std::int64_t limit) {
    const std::int64_t bounded = std::clamp<std::int64_t>(limit, 1, 500);
    return database_.read([&](sql::Connection& connection) {
        std::set<std::string, std::less<>> undo_actions;
        {
            sql::Statement statement = connection.prepare(
                "SELECT meeting_events.action FROM meeting_events WHERE "
                "meeting_events.meeting_id = ? AND meeting_events.action LIKE 'undo:%'");
            statement.bind(1, std::string(meeting_id));
            while (statement.step()) {
                undo_actions.insert(detail::text_value(statement.column(0)));
            }
        }
        std::string sql = "SELECT " + std::string(kEventColumns) +
                          " FROM meeting_events WHERE meeting_events.meeting_id = ?";
        std::vector<sql::Value> params{std::string(meeting_id)};
        if (before_seq) {
            sql += " AND meeting_events.seq < ?";
            params.emplace_back(*before_seq);
        }
        sql += " ORDER BY meeting_events.seq DESC LIMIT ?";
        params.emplace_back(bounded);
        sql::Statement statement = connection.prepare(sql);
        detail::bind_all(statement, params);
        std::vector<Json> events;
        while (statement.step()) {
            Json event = event_row(statement, false);
            // The payload lets the host dashboard describe an event as a sentence.
            const bool has_inverse = !detail::text_value(statement.column(7)).empty();
            event["undoable"] =
                has_inverse && !undo_actions.contains("undo:" + detail::py_str(event.at("seq")));
            events.push_back(std::move(event));
        }
        return events;
    });
}

} // namespace fv::meeting_state
