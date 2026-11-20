// SqlMeetingRepository: transcript segments, the final-transcript replace and agent delivery
// (repository.py add_segments .. get_last_segments, _match_live_to_offline and friends).

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include "db/database.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/detail/sql_internal.h"
#include "meeting_state/schema.h"
#include "meeting_state/sql_repository.h"
#include "meeting_state/time_utils.h"
#include "pyport/errors.h"
#include "pyport/json.h"

namespace fv::meeting_state {

namespace {

// segment_flags asks for this many ids per query.
constexpr std::size_t kFlagsBatch = 500;

const std::string kSelectSegments =
    "SELECT " + std::string(detail::kSegmentColumns) + " FROM meeting_segments";

struct ExistingSegment {
    std::string meeting_id;
    std::optional<std::int64_t> chunk_id;
    sql::Value created_at;
};

std::optional<ExistingSegment> existing_segment(sql::Connection& connection,
                                                std::string_view segment_id) {
    sql::Statement statement = connection.prepare(
        "SELECT meeting_segments.meeting_id, meeting_segments.chunk_id, "
        "meeting_segments.created_at FROM meeting_segments WHERE meeting_segments.id = ?");
    statement.bind(1, std::string(segment_id));
    if (!statement.step()) {
        return std::nullopt;
    }
    ExistingSegment existing;
    existing.meeting_id = detail::text_value(statement.column(0));
    const sql::Value chunk = statement.column(1);
    if (std::holds_alternative<std::int64_t>(chunk)) {
        existing.chunk_id = std::get<std::int64_t>(chunk);
    }
    existing.created_at = statement.column(2);
    return existing;
}

bool participant_in_meeting(sql::Connection& connection, std::string_view meeting_id,
                            const sql::Value& participant_id) {
    sql::Statement statement =
        connection.prepare("SELECT meeting_participants.id FROM meeting_participants WHERE "
                           "meeting_participants.meeting_id = ? AND meeting_participants.id = ? "
                           "LIMIT 1");
    detail::bind_all(statement, {std::string(meeting_id), participant_id});
    return statement.step();
}

bool segment_in_meeting(sql::Connection& connection, std::string_view meeting_id,
                        std::string_view segment_id) {
    sql::Statement statement = connection.prepare(
        "SELECT meeting_segments.id FROM meeting_segments WHERE meeting_segments.meeting_id = ? "
        "AND meeting_segments.id = ? LIMIT 1");
    detail::bind_all(statement, {std::string(meeting_id), std::string(segment_id)});
    return statement.step();
}

std::vector<Json> chunk_segment_rows(sql::Connection& connection, std::string_view meeting_id,
                                     std::int64_t chunk_id) {
    detail::SegmentReader reader(connection);
    return reader.rows(kSelectSegments +
                           " WHERE meeting_segments.meeting_id = ? AND meeting_segments.chunk_id "
                           "= ? ORDER BY meeting_segments.start_s, meeting_segments.id",
                       {std::string(meeting_id), chunk_id});
}

// (start_s, id), the order every transcript read uses.
bool by_time(const Json& a, const Json& b) {
    const double sa = a.at("start_s").is_number() ? a.at("start_s").get<double>() : 0.0;
    const double sb = b.at("start_s").is_number() ? b.at("start_s").get<double>() : 0.0;
    if (sa != sb) {
        return sa < sb;
    }
    return detail::py_str(a.at("id")) < detail::py_str(b.at("id"));
}

const std::string* mapped_id(const std::vector<std::pair<std::string, std::string>>& id_map,
                             std::string_view old_id) {
    for (const auto& [from, to] : id_map) {
        if (from == old_id) {
            return &to;
        }
    }
    return nullptr;
}

// _best_evidence_match: one evidence id onto a unique overlapping new segment.
std::optional<std::string>
best_evidence_match(const std::string& old_id, const std::vector<detail::OldSegment>& old_items,
                    const std::vector<TranscriptSegment>& new_items,
                    const std::vector<std::pair<std::string, std::string>>& id_map,
                    const std::set<std::string, std::less<>>& retained_ids) {
    if (const std::string* to = mapped_id(id_map, old_id)) {
        return *to;
    }
    if (retained_ids.contains(old_id)) {
        return old_id;
    }
    const auto old =
        std::find_if(old_items.begin(), old_items.end(),
                     [&](const detail::OldSegment& item) { return item.id == old_id; });
    if (old == old_items.end()) {
        return std::nullopt;
    }
    std::vector<std::string> matches;
    for (const auto& segment : new_items) {
        if (old->channel != segment.channel) {
            continue;
        }
        if (interval_iou(old->start_s, old->end_s, segment.start_s, segment.end_s) >=
            MIN_MATCH_IOU) {
            matches.push_back(segment.segment_id);
        }
    }
    if (matches.size() != 1) {
        return std::nullopt;
    }
    return matches.front();
}

// _remap_evidence_list: remapped sg_ ids, deduplicated in order; anything else drops.
Json remap_list(const Json& values, const std::vector<detail::OldSegment>& old_items,
                const std::vector<TranscriptSegment>& new_items,
                const std::vector<std::pair<std::string, std::string>>& id_map,
                const std::set<std::string, std::less<>>& retained_ids) {
    Json remapped = Json::array();
    std::set<std::string, std::less<>> seen;
    if (!values.is_array()) {
        return remapped;
    }
    for (const auto& item : values) {
        if (!item.is_string() || !item.get_ref<const std::string&>().starts_with("sg_")) {
            continue;
        }
        const auto mapped = best_evidence_match(item.get<std::string>(), old_items, new_items,
                                                id_map, retained_ids);
        if (mapped && !mapped->empty() && seen.insert(*mapped).second) {
            remapped.push_back(*mapped);
        }
    }
    return remapped;
}

} // namespace

namespace detail {

std::pair<std::vector<std::pair<std::string, std::string>>, std::set<std::size_t>>
match_live_to_offline(const std::vector<OldSegment>& old_items,
                      std::vector<TranscriptSegment>& new_items) {
    std::vector<std::tuple<double, std::size_t, std::size_t>> pairs;
    for (std::size_t oi = 0; oi < old_items.size(); ++oi) {
        for (std::size_t ni = 0; ni < new_items.size(); ++ni) {
            const OldSegment& old = old_items[oi];
            const TranscriptSegment& segment = new_items[ni];
            if (old.channel != segment.channel) {
                continue;
            }
            const double score =
                interval_iou(old.start_s, old.end_s, segment.start_s, segment.end_s);
            if (score >= MIN_MATCH_IOU) {
                pairs.emplace_back(score, oi, ni);
            }
        }
    }
    // pairs.sort(reverse=True): best score first, ties by the later old row, then new row.
    std::stable_sort(pairs.begin(), pairs.end(),
                     [](const auto& a, const auto& b) { return a > b; });
    std::set<std::size_t> used_old;
    std::set<std::size_t> used_new;
    std::vector<std::pair<std::string, std::string>> id_map;
    for (const auto& [score, oi, ni] : pairs) {
        if (used_old.contains(oi) || used_new.contains(ni)) {
            continue;
        }
        used_old.insert(oi);
        used_new.insert(ni);
        const OldSegment& old = old_items[oi];
        TranscriptSegment& segment = new_items[ni];
        if (const auto existing =
                std::find_if(id_map.begin(), id_map.end(),
                             [&](const auto& entry) { return entry.first == old.id; });
            existing != id_map.end()) {
            existing->second = segment.segment_id;
        } else {
            id_map.emplace_back(old.id, segment.segment_id);
        }
        // A human pin always wins. An unpinned live label only fills a gap; otherwise the
        // offline diarization pass is kept.
        const bool old_labeled = old.speaker_participant_id && !old.speaker_participant_id->empty();
        const bool new_labeled =
            segment.speaker_participant_id && !segment.speaker_participant_id->empty();
        if (old.pinned || (old_labeled && !new_labeled)) {
            segment.speaker_participant_id = old.speaker_participant_id;
            segment.speaker_source = old.speaker_source;
            segment.speaker_pinned = old.pinned;
        }
    }
    return {std::move(id_map), std::move(used_old)};
}

void remap_state_evidence(Json& state,
                          const std::vector<std::pair<std::string, std::string>>& id_map,
                          const std::vector<OldSegment>& old_items,
                          const std::vector<TranscriptSegment>& new_items,
                          const std::set<std::string, std::less<>>& retained_ids) {
    const auto remap = [&](const Json& values) {
        return remap_list(values, old_items, new_items, id_map, retained_ids);
    };
    // Proposed items are remapped too: an anchor that survives the re-decode lets the final
    // consolidation reconcile the item instead of rebuilding it; items whose anchors all die
    // end up with empty evidence, which the post-redecode strip uses to drop them.
    if (const auto cards = state.find("cards"); cards != state.end() && cards->is_object()) {
        for (const std::string_view key : CARD_KEYS) {
            const auto found = cards->find(std::string(key));
            if (found == cards->end() || !found->is_array()) {
                continue;
            }
            for (auto& item : *found) {
                if (!item.is_object()) {
                    continue;
                }
                item["evidence"] = remap(get(item, "evidence"));
                item["citation_check"] = Json::object();
            }
        }
    }
    Json pulses = Json::array();
    const Json& highlights = get(state, "live_highlights");
    if (truthy(highlights)) {
        for (const auto& pulse : highlights) {
            const Json mapped = remap(Json::array({get(pulse, "segment_id")}));
            if (!mapped.empty()) {
                Json moved = pulse;
                moved["segment_id"] = mapped.front();
                pulses.push_back(std::move(moved));
            }
        }
    }
    state["live_highlights"] = std::move(pulses);
    if (const auto questions = state.find("questions");
        questions != state.end() && questions->is_array()) {
        for (auto& question : *questions) {
            if (question.is_object()) {
                question["evidence"] = remap(get(question, "evidence"));
            }
        }
    }
    state["rolling_summary_evidence"] = remap(get(state, "rolling_summary_evidence"));
}

} // namespace detail

void SqlMeetingRepository::add_segments(const std::vector<TranscriptSegment>& segments) {
    if (segments.empty()) {
        return;
    }
    const std::string created = utc_now_iso(clock_);
    database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        for (const auto& segment : segments) {
            sql::Statement statement = connection.prepare(
                "INSERT INTO meeting_segments (id, meeting_id, chunk_id, channel, start_s, end_s, "
                "text, speaker_participant_id, speaker_source, speaker_pinned, embedding, "
                "created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
            detail::bind_all(
                statement,
                {segment.segment_id, segment.meeting_id,
                 segment.chunk_id ? sql::Value(*segment.chunk_id) : sql::Value(nullptr),
                 segment.channel, segment.start_s, segment.end_s, segment.text,
                 segment.speaker_participant_id ? sql::Value(*segment.speaker_participant_id)
                                                : sql::Value(nullptr),
                 segment.speaker_source, std::int64_t{segment.speaker_pinned ? 1 : 0},
                 detail::embedding_value(segment.embedding), created});
            statement.step();
        }
        transaction.commit();
    });
}

ChunkCommit
SqlMeetingRepository::commit_chunk_transcription(std::string_view meeting_id, std::int64_t chunk_id,
                                                 const std::vector<TranscriptSegment>& segments) {
    const std::string created = utc_now_iso(clock_);
    return database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        sql::Statement chunk = connection.prepare(
            "SELECT meeting_audio_chunks.asr_status FROM meeting_audio_chunks WHERE "
            "meeting_audio_chunks.id = ? AND meeting_audio_chunks.meeting_id = ?");
        detail::bind_all(chunk, {chunk_id, std::string(meeting_id)});
        if (!chunk.step()) {
            throw py::ValueError("unknown meeting audio chunk");
        }
        if (detail::text_value(chunk.column(0)) == "done") {
            return ChunkCommit{chunk_segment_rows(connection, meeting_id, chunk_id), false};
        }
        for (const auto& segment : segments) {
            if (segment.meeting_id != meeting_id || segment.chunk_id != chunk_id) {
                throw py::ValueError("segment does not belong to chunk");
            }
            const auto existing = existing_segment(connection, segment.segment_id);
            if (existing &&
                (existing->meeting_id != meeting_id || existing->chunk_id != chunk_id)) {
                throw py::ValueError("segment id already belongs to another chunk");
            }
            detail::merge_segment(connection, segment, created);
        }
        sql::Statement done = connection.prepare(
            "UPDATE meeting_audio_chunks SET asr_status = 'done', asr_error = NULL WHERE "
            "meeting_audio_chunks.id = ?");
        done.bind(1, chunk_id);
        done.step();
        ChunkCommit result{chunk_segment_rows(connection, meeting_id, chunk_id), true};
        transaction.commit();
        return result;
    });
}

std::optional<Json> SqlMeetingRepository::get_segment(std::string_view meeting_id,
                                                      std::string_view segment_id) {
    return database_.read([&](sql::Connection& connection) -> std::optional<Json> {
        detail::SegmentReader reader(connection);
        std::vector<Json> rows =
            reader.rows(kSelectSegments + " WHERE meeting_segments.meeting_id = ? AND "
                                          "meeting_segments.id = ?",
                        {std::string(meeting_id), std::string(segment_id)});
        if (rows.empty()) {
            return std::nullopt;
        }
        return std::move(rows.front());
    });
}

bool SqlMeetingRepository::segment_exists(std::string_view meeting_id,
                                          std::string_view segment_id) {
    return database_.read([&](sql::Connection& connection) {
        return segment_in_meeting(connection, meeting_id, segment_id);
    });
}

std::map<std::string, bool, std::less<>>
SqlMeetingRepository::segment_flags(std::string_view meeting_id,
                                    const std::vector<std::string>& segment_ids) {
    std::set<std::string> unique;
    for (const auto& id : segment_ids) {
        if (!id.empty()) {
            unique.insert(id);
        }
    }
    std::map<std::string, bool, std::less<>> flags;
    if (unique.empty()) {
        return flags;
    }
    const std::vector<std::string> ids(unique.begin(), unique.end());
    database_.read([&](sql::Connection& connection) {
        for (std::size_t start = 0; start < ids.size(); start += kFlagsBatch) {
            const std::size_t count = std::min(kFlagsBatch, ids.size() - start);
            sql::Statement statement = connection.prepare(
                "SELECT meeting_segments.id, meeting_segments.speaker_pinned FROM "
                "meeting_segments WHERE meeting_segments.meeting_id = ? AND meeting_segments.id "
                "IN (" +
                detail::placeholders(count) + ")");
            std::vector<sql::Value> params{std::string(meeting_id)};
            for (std::size_t index = start; index < start + count; ++index) {
                params.emplace_back(ids[index]);
            }
            detail::bind_all(statement, params);
            while (statement.step()) {
                flags[detail::text_value(statement.column(0))] =
                    detail::truthy(detail::bool_from_value(statement.column(1)));
            }
        }
    });
    return flags;
}

void SqlMeetingRepository::update_segment_speaker(std::string_view meeting_id,
                                                  std::string_view segment_id,
                                                  const std::optional<std::string>& participant_id,
                                                  std::string_view source, bool pinned) {
    database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        if (!segment_in_meeting(connection, meeting_id, segment_id)) {
            return;
        }
        const sql::Value participant =
            participant_id ? sql::Value(*participant_id) : sql::Value(nullptr);
        if (participant_id && !participant_in_meeting(connection, meeting_id, participant)) {
            throw py::ValueError("participant does not belong to meeting");
        }
        sql::Statement statement = connection.prepare(
            "UPDATE meeting_segments SET speaker_participant_id = ?, speaker_source = ?, "
            "speaker_pinned = ? WHERE meeting_segments.id = ?");
        detail::bind_all(statement, {participant, std::string(source), std::int64_t{pinned ? 1 : 0},
                                     std::string(segment_id)});
        statement.step();
        transaction.commit();
    });
}

std::vector<Json> SqlMeetingRepository::get_segments(std::string_view meeting_id,
                                                     double after_start_s,
                                                     std::optional<std::int64_t> limit) {
    std::string sql = kSelectSegments + " WHERE meeting_segments.meeting_id = ? AND "
                                        "meeting_segments.start_s > ? ORDER BY "
                                        "meeting_segments.start_s, meeting_segments.id";
    std::vector<sql::Value> params{std::string(meeting_id), after_start_s};
    // A zero limit is falsy in Python: unbounded.
    if (limit && *limit != 0) {
        sql += " LIMIT ?";
        params.emplace_back(*limit);
    }
    return database_.read([&](sql::Connection& connection) {
        detail::SegmentReader reader(connection);
        return reader.rows(sql, params);
    });
}

std::vector<Json> SqlMeetingRepository::get_segments_page(
    std::string_view meeting_id, std::optional<double> cursor_start_s,
    const std::optional<std::string>& cursor_id, std::int64_t limit) {
    const std::int64_t bounded = std::clamp<std::int64_t>(limit, 1, 1001);
    std::string sql = kSelectSegments + " WHERE meeting_segments.meeting_id = ?";
    std::vector<sql::Value> params{std::string(meeting_id)};
    if (cursor_start_s && cursor_id) {
        sql += " AND (meeting_segments.start_s > ? OR meeting_segments.start_s = ? AND "
               "meeting_segments.id > ?)";
        params.emplace_back(*cursor_start_s);
        params.emplace_back(*cursor_start_s);
        params.emplace_back(*cursor_id);
    }
    sql += " ORDER BY meeting_segments.start_s, meeting_segments.id LIMIT ?";
    params.emplace_back(bounded);
    return database_.read([&](sql::Connection& connection) {
        detail::SegmentReader reader(connection);
        return reader.rows(sql, params);
    });
}

std::vector<Json> SqlMeetingRepository::get_last_segments(std::string_view meeting_id,
                                                          std::int64_t count) {
    std::vector<Json> rows = database_.read([&](sql::Connection& connection) {
        detail::SegmentReader reader(connection);
        return reader.rows(kSelectSegments +
                               " WHERE meeting_segments.meeting_id = ? ORDER BY "
                               "meeting_segments.start_s DESC, meeting_segments.id DESC LIMIT ?",
                           {std::string(meeting_id), count});
    });
    std::reverse(rows.begin(), rows.end());
    return rows;
}

TranscriptReplacement
SqlMeetingRepository::replace_final_transcript(std::string_view meeting_id,
                                               const std::vector<TranscriptSegment>& segments) {
    const std::string created = utc_now_iso(clock_);
    return database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        std::optional<Json> meeting_state;
        {
            sql::Statement statement =
                connection.prepare("SELECT meeting_sessions.state_json FROM meeting_sessions WHERE "
                                   "meeting_sessions.id = ?");
            statement.bind(1, std::string(meeting_id));
            if (statement.step()) {
                meeting_state = detail::from_value(statement.column(0));
            }
        }
        // Plain snapshots of the live rows, safe to read after they are deleted.
        std::vector<detail::OldSegment> old_items;
        {
            sql::Statement statement = connection.prepare(
                "SELECT meeting_segments.id, meeting_segments.start_s, meeting_segments.end_s, "
                "meeting_segments.speaker_pinned, meeting_segments.speaker_participant_id, "
                "meeting_segments.speaker_source, meeting_segments.channel FROM "
                "meeting_segments WHERE meeting_segments.meeting_id = ?");
            statement.bind(1, std::string(meeting_id));
            while (statement.step()) {
                detail::OldSegment old;
                old.id = detail::text_value(statement.column(0));
                old.start_s = detail::real_value(statement.column(1));
                old.end_s = detail::real_value(statement.column(2));
                old.pinned = detail::truthy(detail::bool_from_value(statement.column(3)));
                const sql::Value speaker = statement.column(4);
                if (std::holds_alternative<std::string>(speaker)) {
                    old.speaker_participant_id = std::get<std::string>(speaker);
                }
                old.speaker_source = detail::text_value(statement.column(5));
                old.channel = detail::text_value(statement.column(6));
                old_items.push_back(std::move(old));
            }
        }
        std::vector<TranscriptSegment> new_items = segments;
        auto [id_map, used_old] = detail::match_live_to_offline(old_items, new_items);

        TranscriptReplacement result;
        std::set<std::string, std::less<>> retained;
        std::vector<std::string> retained_ids;
        for (std::size_t oi = 0; oi < old_items.size(); ++oi) {
            if (old_items[oi].pinned && !used_old.contains(oi)) {
                retained.insert(old_items[oi].id);
                retained_ids.push_back(old_items[oi].id);
            }
        }
        std::set<std::string, std::less<>> incoming;
        for (const auto& segment : new_items) {
            incoming.insert(segment.segment_id);
        }
        // Every live row goes except pinned rows no offline row matched. A row whose id the
        // offline pass reuses is updated in place below, as the ORM turns its delete and add
        // into one update.
        for (const auto& old : old_items) {
            if (retained.contains(old.id)) {
                continue;
            }
            if (!incoming.contains(old.id)) {
                sql::Statement statement = connection.prepare(
                    "DELETE FROM meeting_segments WHERE meeting_segments.id = ?");
                statement.bind(1, old.id);
                statement.step();
            }
            result.deleted_ids.push_back(old.id);
        }
        std::vector<std::string> stored_ids;
        for (const auto& segment : new_items) {
            if (segment.meeting_id != meeting_id) {
                throw py::ValueError("segment does not belong to meeting");
            }
            const auto existing = existing_segment(connection, segment.segment_id);
            if (existing && existing->meeting_id != meeting_id) {
                throw py::ValueError("segment id already belongs to another meeting");
            }
            std::string created_at = created;
            if (existing && std::holds_alternative<std::string>(existing->created_at)) {
                created_at = std::get<std::string>(existing->created_at);
            }
            detail::merge_segment(connection, segment, created_at);
            stored_ids.push_back(segment.segment_id);
        }
        if (meeting_state) {
            if (auto state = parse_state_json(*meeting_state)) {
                detail::remap_state_evidence(*state, id_map, old_items, new_items, retained);
                sql::Statement statement = connection.prepare(
                    "UPDATE meeting_sessions SET state_json = ? WHERE meeting_sessions.id = ?");
                detail::bind_all(statement, {py::dumps(*state, {.ensure_ascii = false}),
                                             std::string(meeting_id)});
                statement.step();
            }
        }
        detail::SegmentReader reader(connection);
        for (const auto* ids : {&stored_ids, &retained_ids}) {
            for (const auto& id : *ids) {
                if (auto row = reader.by_id(id)) {
                    result.rows.push_back(std::move(*row));
                }
            }
        }
        std::stable_sort(result.rows.begin(), result.rows.end(), by_time);
        result.id_map = std::move(id_map);
        transaction.commit();
        return result;
    });
}

std::vector<Json> SqlMeetingRepository::get_segments_delivery_page(std::string_view meeting_id,
                                                                   std::int64_t after_seq,
                                                                   std::int64_t limit) {
    const std::int64_t bounded = std::clamp<std::int64_t>(limit, 1, 301);
    return database_.read([&](sql::Connection& connection) {
        detail::SegmentReader reader(connection);
        sql::Statement statement = connection.prepare(
            "SELECT meeting_segment_delivery.seq, " + std::string(detail::kSegmentColumns) +
            " FROM meeting_segment_delivery JOIN meeting_segments ON meeting_segments.id = "
            "meeting_segment_delivery.segment_id WHERE meeting_segment_delivery.meeting_id = ? "
            "AND meeting_segment_delivery.seq > ? ORDER BY meeting_segment_delivery.seq LIMIT ?");
        detail::bind_all(statement,
                         {std::string(meeting_id), std::max<std::int64_t>(0, after_seq), bounded});
        std::vector<Json> rows;
        while (statement.step()) {
            Json row = reader.dict(statement, 1);
            row["delivery_seq"] = detail::from_value(statement.column(0));
            rows.push_back(std::move(row));
        }
        return rows;
    });
}

std::int64_t SqlMeetingRepository::get_delivery_cursor(std::string_view meeting_id,
                                                       std::string_view consumer) {
    if (consumer != "cards" && consumer != "notes") {
        throw py::ValueError("unknown transcript delivery consumer");
    }
    return database_.read([&](sql::Connection& connection) {
        sql::Statement statement = connection.prepare(
            "SELECT meeting_agent_delivery_cursors.seq FROM meeting_agent_delivery_cursors WHERE "
            "meeting_agent_delivery_cursors.meeting_id = ? AND "
            "meeting_agent_delivery_cursors.consumer = ?");
        detail::bind_all(statement, {std::string(meeting_id), std::string(consumer)});
        return statement.step() ? detail::int_value(statement.column(0)) : std::int64_t{0};
    });
}

void SqlMeetingRepository::advance_delivery_cursor(std::string_view meeting_id,
                                                   std::string_view consumer, std::int64_t seq) {
    if ((consumer != "cards" && consumer != "notes") || seq < 0) {
        throw py::ValueError("invalid transcript delivery cursor");
    }
    database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        // One statement, so concurrent acknowledgements can only move the cursor forward.
        sql::Statement statement = connection.prepare(
            "INSERT INTO meeting_agent_delivery_cursors (meeting_id, consumer, seq) VALUES (?, ?, "
            "?) ON CONFLICT (meeting_id, consumer) DO UPDATE SET seq = "
            "max(meeting_agent_delivery_cursors.seq, excluded.seq)");
        detail::bind_all(statement, {std::string(meeting_id), std::string(consumer), seq});
        statement.step();
        transaction.commit();
    });
}

std::int64_t SqlMeetingRepository::get_delivery_high_water(std::string_view meeting_id) {
    return database_.read([&](sql::Connection& connection) {
        sql::Statement statement = connection.prepare(
            "SELECT max(meeting_segment_delivery.seq) FROM meeting_segment_delivery WHERE "
            "meeting_segment_delivery.meeting_id = ?");
        statement.bind(1, std::string(meeting_id));
        statement.step();
        return detail::int_value(statement.column(0));
    });
}

} // namespace fv::meeting_state
