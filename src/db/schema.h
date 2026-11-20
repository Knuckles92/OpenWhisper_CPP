#pragma once

#include <array>
#include <span>
#include <string_view>

// OpenWhisper's database schema v18 (services/models.py and the raw SQL of
// services/database.py): table and column names for the repositories, and the DDL text exactly
// as SQLAlchemy emits it for SQLite, so a FioriVoce database and an OpenWhisper one hold the same
// sqlite_master. FioriVoce never alters these tables; its own go in fv_* tables.
namespace fv::schema {

// services/database.py SCHEMA_VERSION.
inline constexpr int schema_version = 18;
// The version of the FioriVoce-only fv_* tables, kept in fv_schema_version.
inline constexpr int fv_schema_version = 1;

namespace schema_version_table {
inline constexpr std::string_view table = "schema_version";
inline constexpr std::string_view version = "version";
} // namespace schema_version_table

namespace fv_schema_version_table {
inline constexpr std::string_view table = "fv_schema_version";
inline constexpr std::string_view version = "version";
} // namespace fv_schema_version_table

namespace transcription_history {
inline constexpr std::string_view table = "transcription_history";
inline constexpr std::string_view id = "id";
inline constexpr std::string_view text = "text";
inline constexpr std::string_view raw_text = "raw_text";
inline constexpr std::string_view timestamp = "timestamp";
inline constexpr std::string_view model = "model";
inline constexpr std::string_view audio_file = "audio_file";
inline constexpr std::string_view transcription_time = "transcription_time";
inline constexpr std::string_view audio_duration = "audio_duration";
inline constexpr std::string_view file_size = "file_size";
inline constexpr std::string_view cleanup_provider = "cleanup_provider";
inline constexpr std::string_view cleanup_model = "cleanup_model";
inline constexpr std::string_view source_name = "source_name";
inline constexpr std::string_view title = "title";
inline constexpr std::string_view origin_device_id = "origin_device_id";
inline constexpr std::string_view origin_device_name = "origin_device_name";
inline constexpr std::string_view app_id = "app_id";
inline constexpr std::string_view app_name = "app_name";
inline constexpr std::string_view app_category = "app_category";
inline constexpr std::string_view cleanup_level = "cleanup_level";
inline constexpr std::string_view entry_kind = "entry_kind";
inline constexpr std::string_view cleaned_text = "cleaned_text";
inline constexpr std::string_view language = "language";
// Every column, in table order.
inline constexpr std::array<std::string_view, 22> columns = {id,
                                                             text,
                                                             raw_text,
                                                             timestamp,
                                                             model,
                                                             audio_file,
                                                             transcription_time,
                                                             audio_duration,
                                                             file_size,
                                                             cleanup_provider,
                                                             cleanup_model,
                                                             source_name,
                                                             title,
                                                             origin_device_id,
                                                             origin_device_name,
                                                             app_id,
                                                             app_name,
                                                             app_category,
                                                             cleanup_level,
                                                             entry_kind,
                                                             cleaned_text,
                                                             language};
// services/models.py HISTORY_CONTEXT_COLUMNS: the optional per-entry columns of schema v17.
inline constexpr std::array<std::string_view, 7> context_columns = {
    app_id, app_name, app_category, cleanup_level, entry_kind, cleaned_text, language};
inline constexpr std::string_view timestamp_index = "idx_history_timestamp";
} // namespace transcription_history

namespace dictation_stats {
inline constexpr std::string_view table = "dictation_stats";
inline constexpr std::string_view id = "id";
inline constexpr std::string_view entry_id = "entry_id";
inline constexpr std::string_view timestamp = "timestamp";
inline constexpr std::string_view entry_kind = "entry_kind";
inline constexpr std::string_view app_name = "app_name";
inline constexpr std::string_view app_category = "app_category";
inline constexpr std::string_view cleanup_level = "cleanup_level";
inline constexpr std::string_view spoken_words = "spoken_words";
inline constexpr std::string_view final_words = "final_words";
inline constexpr std::string_view words_edited = "words_edited";
inline constexpr std::string_view audio_seconds = "audio_seconds";
inline constexpr std::string_view language = "language";
inline constexpr std::string_view target = "target";
inline constexpr std::string_view snippets = "snippets";
inline constexpr std::string_view snippet_words = "snippet_words";
inline constexpr std::string_view corrections = "corrections";
inline constexpr std::string_view shots = "shots";
inline constexpr std::string_view timestamp_index = "idx_dictation_stats_timestamp";
} // namespace dictation_stats

namespace meeting_sessions {
inline constexpr std::string_view table = "meeting_sessions";
inline constexpr std::string_view id = "id";
inline constexpr std::string_view title = "title";
inline constexpr std::string_view status = "status";
inline constexpr std::string_view started_at = "started_at";
inline constexpr std::string_view ended_at = "ended_at";
inline constexpr std::string_view paused_total_s = "paused_total_s";
inline constexpr std::string_view host_token = "host_token";
inline constexpr std::string_view guest_token = "guest_token";
inline constexpr std::string_view cloud_enabled = "cloud_enabled";
inline constexpr std::string_view asr_model = "asr_model";
inline constexpr std::string_view asr_remote_json = "asr_remote_json";
inline constexpr std::string_view agent_provider = "agent_provider";
inline constexpr std::string_view agent_model = "agent_model";
inline constexpr std::string_view agent_endpoint_json = "agent_endpoint_json";
inline constexpr std::string_view spool_dir = "spool_dir";
inline constexpr std::string_view state_json = "state_json";
inline constexpr std::string_view state_seq = "state_seq";
inline constexpr std::string_view app_pid = "app_pid";
inline constexpr std::string_view app_heartbeat_at = "app_heartbeat_at";
inline constexpr std::string_view origin_device_id = "origin_device_id";
inline constexpr std::string_view origin_device_name = "origin_device_name";
inline constexpr std::string_view started_index = "idx_msessions_started";
} // namespace meeting_sessions

namespace record_sync {
inline constexpr std::string_view table = "record_sync";
inline constexpr std::string_view kind = "kind";
inline constexpr std::string_view record_id = "record_id";
inline constexpr std::string_view host_fingerprint = "host_fingerprint";
inline constexpr std::string_view action = "action";
inline constexpr std::string_view state = "state";
inline constexpr std::string_view attempts = "attempts";
inline constexpr std::string_view last_error = "last_error";
inline constexpr std::string_view content_digest = "content_digest";
inline constexpr std::string_view created_at = "created_at";
inline constexpr std::string_view updated_at = "updated_at";
inline constexpr std::string_view state_index = "idx_record_sync_state";
} // namespace record_sync

namespace meeting_audio_chunks {
inline constexpr std::string_view table = "meeting_audio_chunks";
inline constexpr std::string_view id = "id";
inline constexpr std::string_view meeting_id = "meeting_id";
inline constexpr std::string_view channel = "channel";
inline constexpr std::string_view seq = "seq";
inline constexpr std::string_view file_path = "file_path";
inline constexpr std::string_view start_s = "start_s";
inline constexpr std::string_view duration_s = "duration_s";
inline constexpr std::string_view sample_rate = "sample_rate";
inline constexpr std::string_view asr_status = "asr_status";
inline constexpr std::string_view asr_attempts = "asr_attempts";
inline constexpr std::string_view asr_error = "asr_error";
inline constexpr std::string_view seq_constraint = "uq_mchunks_seq";
inline constexpr std::string_view pending_index = "idx_mchunks_pending";
} // namespace meeting_audio_chunks

namespace meeting_agent_delivery_cursors {
inline constexpr std::string_view table = "meeting_agent_delivery_cursors";
inline constexpr std::string_view meeting_id = "meeting_id";
inline constexpr std::string_view consumer = "consumer";
inline constexpr std::string_view seq = "seq";
} // namespace meeting_agent_delivery_cursors

namespace meeting_participants {
inline constexpr std::string_view table = "meeting_participants";
inline constexpr std::string_view id = "id";
inline constexpr std::string_view meeting_id = "meeting_id";
inline constexpr std::string_view display_name = "display_name";
inline constexpr std::string_view kind = "kind";
inline constexpr std::string_view name_source = "name_source";
inline constexpr std::string_view is_provisional = "is_provisional";
inline constexpr std::string_view created_at = "created_at";
inline constexpr std::string_view updated_at = "updated_at";
} // namespace meeting_participants

namespace meeting_state_items {
inline constexpr std::string_view table = "meeting_state_items";
inline constexpr std::string_view id = "id";
inline constexpr std::string_view meeting_id = "meeting_id";
inline constexpr std::string_view card = "card";
inline constexpr std::string_view text = "text";
inline constexpr std::string_view data_json = "data_json";
inline constexpr std::string_view status = "status";
inline constexpr std::string_view author_type = "author_type";
inline constexpr std::string_view author_id = "author_id";
inline constexpr std::string_view pinned = "pinned";
inline constexpr std::string_view revision = "revision";
inline constexpr std::string_view evidence_json = "evidence_json";
inline constexpr std::string_view created_at = "created_at";
inline constexpr std::string_view updated_at = "updated_at";
inline constexpr std::string_view meeting_index = "idx_mitems_meeting";
} // namespace meeting_state_items

namespace meeting_questions {
inline constexpr std::string_view table = "meeting_questions";
inline constexpr std::string_view id = "id";
inline constexpr std::string_view meeting_id = "meeting_id";
inline constexpr std::string_view text = "text";
inline constexpr std::string_view status = "status";
inline constexpr std::string_view answer_text = "answer_text";
inline constexpr std::string_view answer_source = "answer_source";
inline constexpr std::string_view answer_confidence = "answer_confidence";
inline constexpr std::string_view suggested_answer_text = "suggested_answer_text";
inline constexpr std::string_view suggested_confidence = "suggested_confidence";
inline constexpr std::string_view thread_json = "thread_json";
inline constexpr std::string_view evidence_json = "evidence_json";
inline constexpr std::string_view asked_at = "asked_at";
inline constexpr std::string_view resolved_at = "resolved_at";
inline constexpr std::string_view resolved_by = "resolved_by";
} // namespace meeting_questions

namespace meeting_events {
inline constexpr std::string_view table = "meeting_events";
inline constexpr std::string_view id = "id";
inline constexpr std::string_view meeting_id = "meeting_id";
inline constexpr std::string_view seq = "seq";
inline constexpr std::string_view ts = "ts";
inline constexpr std::string_view actor_type = "actor_type";
inline constexpr std::string_view actor_id = "actor_id";
inline constexpr std::string_view action = "action";
inline constexpr std::string_view target_id = "target_id";
inline constexpr std::string_view payload_json = "payload_json";
inline constexpr std::string_view inverse_json = "inverse_json";
inline constexpr std::string_view meeting_index = "idx_mevents_meeting";
} // namespace meeting_events

namespace meeting_segments {
inline constexpr std::string_view table = "meeting_segments";
inline constexpr std::string_view id = "id";
inline constexpr std::string_view meeting_id = "meeting_id";
inline constexpr std::string_view chunk_id = "chunk_id";
inline constexpr std::string_view channel = "channel";
inline constexpr std::string_view start_s = "start_s";
inline constexpr std::string_view end_s = "end_s";
inline constexpr std::string_view text = "text";
inline constexpr std::string_view speaker_participant_id = "speaker_participant_id";
inline constexpr std::string_view speaker_source = "speaker_source";
inline constexpr std::string_view speaker_pinned = "speaker_pinned";
inline constexpr std::string_view embedding = "embedding"; // float32 bytes
inline constexpr std::string_view created_at = "created_at";
inline constexpr std::string_view time_index = "idx_msegments_time";
// The FTS5 external-content index over text, and its sync triggers.
inline constexpr std::string_view fts_table = "meeting_segments_fts";
inline constexpr std::string_view fts_insert_trigger = "meeting_segments_fts_ai";
inline constexpr std::string_view fts_delete_trigger = "meeting_segments_fts_ad";
inline constexpr std::string_view fts_update_trigger = "meeting_segments_fts_au";
// Fills meeting_segment_delivery on every insert.
inline constexpr std::string_view delivery_trigger = "meeting_segments_delivery_ai";
} // namespace meeting_segments

namespace meeting_segment_delivery {
inline constexpr std::string_view table = "meeting_segment_delivery";
inline constexpr std::string_view seq = "seq"; // AUTOINCREMENT: never reused
inline constexpr std::string_view segment_id = "segment_id";
inline constexpr std::string_view meeting_id = "meeting_id";
inline constexpr std::string_view page_index = "idx_msegment_delivery_page";
} // namespace meeting_segment_delivery

// Meeting tables of OpenWhisper before schema v7, dropped on every open.
inline constexpr std::array<std::string_view, 3> removed_meeting_tables = {
    "meeting_insights", "meeting_chunks", "meetings"};

// One table of Base.metadata: its CREATE TABLE and the CREATE INDEX statements that follow it.
struct TableDdl {
    std::string_view name;
    std::string_view create;
    std::span<const std::string_view> indexes;
};

// Base.metadata.create_all's tables in its creation order (dependencies first).
std::span<const TableDdl> tables();
// The raw SQL of DatabaseManager._ensure_meeting_fts, in order.
std::span<const std::string_view> meeting_fts_statements();
// The CREATE TRIGGER IF NOT EXISTS of DatabaseManager._ensure_meeting_delivery.
std::string_view meeting_delivery_trigger();

} // namespace fv::schema
