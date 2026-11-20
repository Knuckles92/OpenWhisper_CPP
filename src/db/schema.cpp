#include "db/schema.h"

#include <array>

namespace fv::schema {

namespace {

// sqlite_master.sql of a database OpenWhisper's Base.metadata.create_all made (SQLAlchemy 2 on
// SQLite): tab indents, ", " before each line break, DEFAULT before NOT NULL. Byte for byte, so a
// FioriVoce database and an OpenWhisper one hold the same schema text (tests/db/test_golden.cpp).

constexpr std::string_view schema_version_create = "CREATE TABLE schema_version (\n"
                                                   "\tversion INTEGER NOT NULL, \n"
                                                   "\tPRIMARY KEY (version)\n"
                                                   ")";

constexpr std::string_view transcription_history_create = "CREATE TABLE transcription_history (\n"
                                                          "\tid VARCHAR NOT NULL, \n"
                                                          "\ttext TEXT NOT NULL, \n"
                                                          "\traw_text TEXT, \n"
                                                          "\ttimestamp VARCHAR NOT NULL, \n"
                                                          "\tmodel VARCHAR NOT NULL, \n"
                                                          "\taudio_file VARCHAR, \n"
                                                          "\ttranscription_time FLOAT, \n"
                                                          "\taudio_duration FLOAT, \n"
                                                          "\tfile_size INTEGER, \n"
                                                          "\tcleanup_provider VARCHAR, \n"
                                                          "\tcleanup_model VARCHAR, \n"
                                                          "\tsource_name VARCHAR, \n"
                                                          "\ttitle VARCHAR, \n"
                                                          "\torigin_device_id VARCHAR, \n"
                                                          "\torigin_device_name VARCHAR, \n"
                                                          "\tapp_id VARCHAR, \n"
                                                          "\tapp_name VARCHAR, \n"
                                                          "\tapp_category VARCHAR, \n"
                                                          "\tcleanup_level VARCHAR, \n"
                                                          "\tentry_kind VARCHAR, \n"
                                                          "\tcleaned_text TEXT, \n"
                                                          "\tlanguage VARCHAR, \n"
                                                          "\tPRIMARY KEY (id)\n"
                                                          ")";

constexpr std::string_view idx_history_timestamp_create =
    "CREATE INDEX idx_history_timestamp ON transcription_history (timestamp)";

constexpr std::string_view dictation_stats_create =
    "CREATE TABLE dictation_stats (\n"
    "\tid INTEGER NOT NULL, \n"
    "\tentry_id VARCHAR, \n"
    "\ttimestamp VARCHAR NOT NULL, \n"
    "\tentry_kind VARCHAR NOT NULL, \n"
    "\tapp_name VARCHAR, \n"
    "\tapp_category VARCHAR, \n"
    "\tcleanup_level VARCHAR, \n"
    "\tspoken_words INTEGER NOT NULL, \n"
    "\tfinal_words INTEGER NOT NULL, \n"
    "\twords_edited INTEGER NOT NULL, \n"
    "\taudio_seconds FLOAT, \n"
    "\tlanguage VARCHAR, \n"
    "\ttarget VARCHAR, \n"
    "\tsnippets INTEGER DEFAULT '0' NOT NULL, \n"
    "\tsnippet_words INTEGER DEFAULT '0' NOT NULL, \n"
    "\tcorrections INTEGER DEFAULT '0' NOT NULL, \n"
    "\tshots INTEGER DEFAULT '0' NOT NULL, \n"
    "\tPRIMARY KEY (id)\n"
    ")";

constexpr std::string_view idx_dictation_stats_timestamp_create =
    "CREATE INDEX idx_dictation_stats_timestamp ON dictation_stats (timestamp)";

constexpr std::string_view meeting_sessions_create = "CREATE TABLE meeting_sessions (\n"
                                                     "\tid VARCHAR NOT NULL, \n"
                                                     "\ttitle VARCHAR NOT NULL, \n"
                                                     "\tstatus VARCHAR NOT NULL, \n"
                                                     "\tstarted_at VARCHAR NOT NULL, \n"
                                                     "\tended_at VARCHAR, \n"
                                                     "\tpaused_total_s FLOAT NOT NULL, \n"
                                                     "\thost_token VARCHAR NOT NULL, \n"
                                                     "\tguest_token VARCHAR NOT NULL, \n"
                                                     "\tcloud_enabled BOOLEAN NOT NULL, \n"
                                                     "\tasr_model VARCHAR, \n"
                                                     "\tasr_remote_json TEXT, \n"
                                                     "\tagent_provider VARCHAR, \n"
                                                     "\tagent_model VARCHAR, \n"
                                                     "\tagent_endpoint_json TEXT, \n"
                                                     "\tspool_dir VARCHAR NOT NULL, \n"
                                                     "\tstate_json TEXT, \n"
                                                     "\tstate_seq INTEGER NOT NULL, \n"
                                                     "\tapp_pid INTEGER, \n"
                                                     "\tapp_heartbeat_at VARCHAR, \n"
                                                     "\torigin_device_id VARCHAR, \n"
                                                     "\torigin_device_name VARCHAR, \n"
                                                     "\tPRIMARY KEY (id)\n"
                                                     ")";

constexpr std::string_view idx_msessions_started_create =
    "CREATE INDEX idx_msessions_started ON meeting_sessions (started_at)";

constexpr std::string_view record_sync_create = "CREATE TABLE record_sync (\n"
                                                "\tkind VARCHAR NOT NULL, \n"
                                                "\trecord_id VARCHAR NOT NULL, \n"
                                                "\thost_fingerprint VARCHAR NOT NULL, \n"
                                                "\taction VARCHAR NOT NULL, \n"
                                                "\tstate VARCHAR NOT NULL, \n"
                                                "\tattempts INTEGER NOT NULL, \n"
                                                "\tlast_error TEXT, \n"
                                                "\tcontent_digest VARCHAR, \n"
                                                "\tcreated_at VARCHAR NOT NULL, \n"
                                                "\tupdated_at VARCHAR NOT NULL, \n"
                                                "\tPRIMARY KEY (kind, record_id)\n"
                                                ")";

constexpr std::string_view idx_record_sync_state_create =
    "CREATE INDEX idx_record_sync_state ON record_sync (state)";

constexpr std::string_view meeting_audio_chunks_create =
    "CREATE TABLE meeting_audio_chunks (\n"
    "\tid INTEGER NOT NULL, \n"
    "\tmeeting_id VARCHAR NOT NULL, \n"
    "\tchannel VARCHAR NOT NULL, \n"
    "\tseq INTEGER NOT NULL, \n"
    "\tfile_path VARCHAR NOT NULL, \n"
    "\tstart_s FLOAT NOT NULL, \n"
    "\tduration_s FLOAT NOT NULL, \n"
    "\tsample_rate INTEGER NOT NULL, \n"
    "\tasr_status VARCHAR NOT NULL, \n"
    "\tasr_attempts INTEGER NOT NULL, \n"
    "\tasr_error TEXT, \n"
    "\tPRIMARY KEY (id), \n"
    "\tCONSTRAINT uq_mchunks_seq UNIQUE (meeting_id, channel, seq), \n"
    "\tFOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE\n"
    ")";

constexpr std::string_view idx_mchunks_pending_create =
    "CREATE INDEX idx_mchunks_pending ON meeting_audio_chunks (meeting_id, asr_status)";

constexpr std::string_view meeting_agent_delivery_cursors_create =
    "CREATE TABLE meeting_agent_delivery_cursors (\n"
    "\tmeeting_id VARCHAR NOT NULL, \n"
    "\tconsumer VARCHAR NOT NULL, \n"
    "\tseq INTEGER NOT NULL, \n"
    "\tPRIMARY KEY (meeting_id, consumer), \n"
    "\tFOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE\n"
    ")";

constexpr std::string_view meeting_participants_create =
    "CREATE TABLE meeting_participants (\n"
    "\tid VARCHAR NOT NULL, \n"
    "\tmeeting_id VARCHAR NOT NULL, \n"
    "\tdisplay_name VARCHAR NOT NULL, \n"
    "\tkind VARCHAR NOT NULL, \n"
    "\tname_source VARCHAR NOT NULL, \n"
    "\tis_provisional BOOLEAN NOT NULL, \n"
    "\tcreated_at VARCHAR NOT NULL, \n"
    "\tupdated_at VARCHAR NOT NULL, \n"
    "\tPRIMARY KEY (id), \n"
    "\tFOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE\n"
    ")";

constexpr std::string_view meeting_state_items_create =
    "CREATE TABLE meeting_state_items (\n"
    "\tid VARCHAR NOT NULL, \n"
    "\tmeeting_id VARCHAR NOT NULL, \n"
    "\tcard VARCHAR NOT NULL, \n"
    "\ttext TEXT NOT NULL, \n"
    "\tdata_json TEXT, \n"
    "\tstatus VARCHAR NOT NULL, \n"
    "\tauthor_type VARCHAR NOT NULL, \n"
    "\tauthor_id VARCHAR, \n"
    "\tpinned BOOLEAN NOT NULL, \n"
    "\trevision INTEGER NOT NULL, \n"
    "\tevidence_json TEXT NOT NULL, \n"
    "\tcreated_at VARCHAR NOT NULL, \n"
    "\tupdated_at VARCHAR NOT NULL, \n"
    "\tPRIMARY KEY (id), \n"
    "\tFOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE\n"
    ")";

constexpr std::string_view idx_mitems_meeting_create =
    "CREATE INDEX idx_mitems_meeting ON meeting_state_items (meeting_id)";

constexpr std::string_view meeting_questions_create =
    "CREATE TABLE meeting_questions (\n"
    "\tid VARCHAR NOT NULL, \n"
    "\tmeeting_id VARCHAR NOT NULL, \n"
    "\ttext TEXT NOT NULL, \n"
    "\tstatus VARCHAR NOT NULL, \n"
    "\tanswer_text TEXT, \n"
    "\tanswer_source VARCHAR, \n"
    "\tanswer_confidence FLOAT, \n"
    "\tsuggested_answer_text TEXT, \n"
    "\tsuggested_confidence FLOAT, \n"
    "\tthread_json TEXT NOT NULL, \n"
    "\tevidence_json TEXT NOT NULL, \n"
    "\tasked_at VARCHAR NOT NULL, \n"
    "\tresolved_at VARCHAR, \n"
    "\tresolved_by VARCHAR, \n"
    "\tPRIMARY KEY (id), \n"
    "\tFOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE\n"
    ")";

constexpr std::string_view meeting_events_create =
    "CREATE TABLE meeting_events (\n"
    "\tid INTEGER NOT NULL, \n"
    "\tmeeting_id VARCHAR NOT NULL, \n"
    "\tseq INTEGER NOT NULL, \n"
    "\tts VARCHAR NOT NULL, \n"
    "\tactor_type VARCHAR NOT NULL, \n"
    "\tactor_id VARCHAR, \n"
    "\taction VARCHAR NOT NULL, \n"
    "\ttarget_id VARCHAR, \n"
    "\tpayload_json TEXT NOT NULL, \n"
    "\tinverse_json TEXT, \n"
    "\tPRIMARY KEY (id), \n"
    "\tFOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE\n"
    ")";

constexpr std::string_view idx_mevents_meeting_create =
    "CREATE INDEX idx_mevents_meeting ON meeting_events (meeting_id, seq)";

constexpr std::string_view meeting_segments_create =
    "CREATE TABLE meeting_segments (\n"
    "\tid VARCHAR NOT NULL, \n"
    "\tmeeting_id VARCHAR NOT NULL, \n"
    "\tchunk_id INTEGER, \n"
    "\tchannel VARCHAR NOT NULL, \n"
    "\tstart_s FLOAT NOT NULL, \n"
    "\tend_s FLOAT NOT NULL, \n"
    "\ttext TEXT NOT NULL, \n"
    "\tspeaker_participant_id VARCHAR, \n"
    "\tspeaker_source VARCHAR NOT NULL, \n"
    "\tspeaker_pinned BOOLEAN NOT NULL, \n"
    "\tembedding BLOB, \n"
    "\tcreated_at VARCHAR NOT NULL, \n"
    "\tPRIMARY KEY (id), \n"
    "\tFOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE, \n"
    "\tFOREIGN KEY(chunk_id) REFERENCES meeting_audio_chunks (id)\n"
    ")";

constexpr std::string_view idx_msegments_time_create =
    "CREATE INDEX idx_msegments_time ON meeting_segments (meeting_id, start_s)";

constexpr std::string_view meeting_segment_delivery_create =
    "CREATE TABLE meeting_segment_delivery (\n"
    "\tseq INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, \n"
    "\tsegment_id VARCHAR NOT NULL, \n"
    "\tmeeting_id VARCHAR NOT NULL, \n"
    "\tUNIQUE (segment_id), \n"
    "\tFOREIGN KEY(segment_id) REFERENCES meeting_segments (id) ON DELETE CASCADE, \n"
    "\tFOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE\n"
    ")";

constexpr std::string_view idx_msegment_delivery_page_create =
    "CREATE INDEX idx_msegment_delivery_page ON meeting_segment_delivery (meeting_id, seq)";

constexpr std::array<std::string_view, 1> transcription_history_indexes = {
    idx_history_timestamp_create};
constexpr std::array<std::string_view, 1> dictation_stats_indexes = {
    idx_dictation_stats_timestamp_create};
constexpr std::array<std::string_view, 1> meeting_sessions_indexes = {idx_msessions_started_create};
constexpr std::array<std::string_view, 1> record_sync_indexes = {idx_record_sync_state_create};
constexpr std::array<std::string_view, 1> meeting_audio_chunks_indexes = {
    idx_mchunks_pending_create};
constexpr std::array<std::string_view, 1> meeting_state_items_indexes = {idx_mitems_meeting_create};
constexpr std::array<std::string_view, 1> meeting_events_indexes = {idx_mevents_meeting_create};
constexpr std::array<std::string_view, 1> meeting_segments_indexes = {idx_msegments_time_create};
constexpr std::array<std::string_view, 1> meeting_segment_delivery_indexes = {
    idx_msegment_delivery_page_create};

constexpr std::array<TableDdl, 13> all_tables = {{
    {schema_version_table::table, schema_version_create, {}},
    {transcription_history::table, transcription_history_create, transcription_history_indexes},
    {dictation_stats::table, dictation_stats_create, dictation_stats_indexes},
    {meeting_sessions::table, meeting_sessions_create, meeting_sessions_indexes},
    {record_sync::table, record_sync_create, record_sync_indexes},
    {meeting_audio_chunks::table, meeting_audio_chunks_create, meeting_audio_chunks_indexes},
    {meeting_agent_delivery_cursors::table, meeting_agent_delivery_cursors_create, {}},
    {meeting_participants::table, meeting_participants_create, {}},
    {meeting_state_items::table, meeting_state_items_create, meeting_state_items_indexes},
    {meeting_questions::table, meeting_questions_create, {}},
    {meeting_events::table, meeting_events_create, meeting_events_indexes},
    {meeting_segments::table, meeting_segments_create, meeting_segments_indexes},
    {meeting_segment_delivery::table, meeting_segment_delivery_create,
     meeting_segment_delivery_indexes},
}};

// services/database.py _ensure_meeting_fts. The indentation is Python's and is stored as written;
// SQLite drops "IF NOT EXISTS" from the stored text.
constexpr std::array<std::string_view, 4> fts_statements = {
    "CREATE VIRTUAL TABLE IF NOT EXISTS meeting_segments_fts USING fts5(\n"
    "                text, content='meeting_segments', content_rowid='rowid'\n"
    "            )",
    "CREATE TRIGGER IF NOT EXISTS meeting_segments_fts_ai\n"
    "            AFTER INSERT ON meeting_segments BEGIN\n"
    "                INSERT INTO meeting_segments_fts(rowid, text)\n"
    "                VALUES (new.rowid, new.text);\n"
    "            END",
    "CREATE TRIGGER IF NOT EXISTS meeting_segments_fts_ad\n"
    "            AFTER DELETE ON meeting_segments BEGIN\n"
    "                INSERT INTO meeting_segments_fts(meeting_segments_fts, rowid, text)\n"
    "                VALUES ('delete', old.rowid, old.text);\n"
    "            END",
    "CREATE TRIGGER IF NOT EXISTS meeting_segments_fts_au\n"
    "            AFTER UPDATE OF text ON meeting_segments BEGIN\n"
    "                INSERT INTO meeting_segments_fts(meeting_segments_fts, rowid, text)\n"
    "                VALUES ('delete', old.rowid, old.text);\n"
    "                INSERT INTO meeting_segments_fts(rowid, text)\n"
    "                VALUES (new.rowid, new.text);\n"
    "            END",
};

// services/database.py _ensure_meeting_delivery.
constexpr std::string_view delivery_trigger =
    "CREATE TRIGGER IF NOT EXISTS meeting_segments_delivery_ai\n"
    "                AFTER INSERT ON meeting_segments BEGIN\n"
    "                    INSERT INTO meeting_segment_delivery(segment_id, meeting_id)\n"
    "                    VALUES (new.id, new.meeting_id);\n"
    "                END";

} // namespace

std::span<const TableDdl> tables() {
    return all_tables;
}

std::span<const std::string_view> meeting_fts_statements() {
    return fts_statements;
}

std::string_view meeting_delivery_trigger() {
    return delivery_trigger;
}

} // namespace fv::schema
