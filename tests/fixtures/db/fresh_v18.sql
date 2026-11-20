CREATE TABLE schema_version (
	version INTEGER NOT NULL, 
	PRIMARY KEY (version)
);

CREATE TABLE transcription_history (
	id VARCHAR NOT NULL, 
	text TEXT NOT NULL, 
	raw_text TEXT, 
	timestamp VARCHAR NOT NULL, 
	model VARCHAR NOT NULL, 
	audio_file VARCHAR, 
	transcription_time FLOAT, 
	audio_duration FLOAT, 
	file_size INTEGER, 
	cleanup_provider VARCHAR, 
	cleanup_model VARCHAR, 
	source_name VARCHAR, 
	title VARCHAR, 
	origin_device_id VARCHAR, 
	origin_device_name VARCHAR, 
	app_id VARCHAR, 
	app_name VARCHAR, 
	app_category VARCHAR, 
	cleanup_level VARCHAR, 
	entry_kind VARCHAR, 
	cleaned_text TEXT, 
	language VARCHAR, 
	PRIMARY KEY (id)
);

CREATE INDEX idx_history_timestamp ON transcription_history (timestamp);

CREATE TABLE dictation_stats (
	id INTEGER NOT NULL, 
	entry_id VARCHAR, 
	timestamp VARCHAR NOT NULL, 
	entry_kind VARCHAR NOT NULL, 
	app_name VARCHAR, 
	app_category VARCHAR, 
	cleanup_level VARCHAR, 
	spoken_words INTEGER NOT NULL, 
	final_words INTEGER NOT NULL, 
	words_edited INTEGER NOT NULL, 
	audio_seconds FLOAT, 
	language VARCHAR, 
	target VARCHAR, 
	snippets INTEGER DEFAULT '0' NOT NULL, 
	snippet_words INTEGER DEFAULT '0' NOT NULL, 
	corrections INTEGER DEFAULT '0' NOT NULL, 
	shots INTEGER DEFAULT '0' NOT NULL, 
	PRIMARY KEY (id)
);

CREATE INDEX idx_dictation_stats_timestamp ON dictation_stats (timestamp);

CREATE TABLE meeting_sessions (
	id VARCHAR NOT NULL, 
	title VARCHAR NOT NULL, 
	status VARCHAR NOT NULL, 
	started_at VARCHAR NOT NULL, 
	ended_at VARCHAR, 
	paused_total_s FLOAT NOT NULL, 
	host_token VARCHAR NOT NULL, 
	guest_token VARCHAR NOT NULL, 
	cloud_enabled BOOLEAN NOT NULL, 
	asr_model VARCHAR, 
	asr_remote_json TEXT, 
	agent_provider VARCHAR, 
	agent_model VARCHAR, 
	agent_endpoint_json TEXT, 
	spool_dir VARCHAR NOT NULL, 
	state_json TEXT, 
	state_seq INTEGER NOT NULL, 
	app_pid INTEGER, 
	app_heartbeat_at VARCHAR, 
	origin_device_id VARCHAR, 
	origin_device_name VARCHAR, 
	PRIMARY KEY (id)
);

CREATE INDEX idx_msessions_started ON meeting_sessions (started_at);

CREATE TABLE record_sync (
	kind VARCHAR NOT NULL, 
	record_id VARCHAR NOT NULL, 
	host_fingerprint VARCHAR NOT NULL, 
	action VARCHAR NOT NULL, 
	state VARCHAR NOT NULL, 
	attempts INTEGER NOT NULL, 
	last_error TEXT, 
	content_digest VARCHAR, 
	created_at VARCHAR NOT NULL, 
	updated_at VARCHAR NOT NULL, 
	PRIMARY KEY (kind, record_id)
);

CREATE INDEX idx_record_sync_state ON record_sync (state);

CREATE TABLE meeting_audio_chunks (
	id INTEGER NOT NULL, 
	meeting_id VARCHAR NOT NULL, 
	channel VARCHAR NOT NULL, 
	seq INTEGER NOT NULL, 
	file_path VARCHAR NOT NULL, 
	start_s FLOAT NOT NULL, 
	duration_s FLOAT NOT NULL, 
	sample_rate INTEGER NOT NULL, 
	asr_status VARCHAR NOT NULL, 
	asr_attempts INTEGER NOT NULL, 
	asr_error TEXT, 
	PRIMARY KEY (id), 
	CONSTRAINT uq_mchunks_seq UNIQUE (meeting_id, channel, seq), 
	FOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE
);

CREATE INDEX idx_mchunks_pending ON meeting_audio_chunks (meeting_id, asr_status);

CREATE TABLE meeting_agent_delivery_cursors (
	meeting_id VARCHAR NOT NULL, 
	consumer VARCHAR NOT NULL, 
	seq INTEGER NOT NULL, 
	PRIMARY KEY (meeting_id, consumer), 
	FOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE
);

CREATE TABLE meeting_participants (
	id VARCHAR NOT NULL, 
	meeting_id VARCHAR NOT NULL, 
	display_name VARCHAR NOT NULL, 
	kind VARCHAR NOT NULL, 
	name_source VARCHAR NOT NULL, 
	is_provisional BOOLEAN NOT NULL, 
	created_at VARCHAR NOT NULL, 
	updated_at VARCHAR NOT NULL, 
	PRIMARY KEY (id), 
	FOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE
);

CREATE TABLE meeting_state_items (
	id VARCHAR NOT NULL, 
	meeting_id VARCHAR NOT NULL, 
	card VARCHAR NOT NULL, 
	text TEXT NOT NULL, 
	data_json TEXT, 
	status VARCHAR NOT NULL, 
	author_type VARCHAR NOT NULL, 
	author_id VARCHAR, 
	pinned BOOLEAN NOT NULL, 
	revision INTEGER NOT NULL, 
	evidence_json TEXT NOT NULL, 
	created_at VARCHAR NOT NULL, 
	updated_at VARCHAR NOT NULL, 
	PRIMARY KEY (id), 
	FOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE
);

CREATE INDEX idx_mitems_meeting ON meeting_state_items (meeting_id);

CREATE TABLE meeting_questions (
	id VARCHAR NOT NULL, 
	meeting_id VARCHAR NOT NULL, 
	text TEXT NOT NULL, 
	status VARCHAR NOT NULL, 
	answer_text TEXT, 
	answer_source VARCHAR, 
	answer_confidence FLOAT, 
	suggested_answer_text TEXT, 
	suggested_confidence FLOAT, 
	thread_json TEXT NOT NULL, 
	evidence_json TEXT NOT NULL, 
	asked_at VARCHAR NOT NULL, 
	resolved_at VARCHAR, 
	resolved_by VARCHAR, 
	PRIMARY KEY (id), 
	FOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE
);

CREATE TABLE meeting_events (
	id INTEGER NOT NULL, 
	meeting_id VARCHAR NOT NULL, 
	seq INTEGER NOT NULL, 
	ts VARCHAR NOT NULL, 
	actor_type VARCHAR NOT NULL, 
	actor_id VARCHAR, 
	action VARCHAR NOT NULL, 
	target_id VARCHAR, 
	payload_json TEXT NOT NULL, 
	inverse_json TEXT, 
	PRIMARY KEY (id), 
	FOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE
);

CREATE INDEX idx_mevents_meeting ON meeting_events (meeting_id, seq);

CREATE TABLE meeting_segments (
	id VARCHAR NOT NULL, 
	meeting_id VARCHAR NOT NULL, 
	chunk_id INTEGER, 
	channel VARCHAR NOT NULL, 
	start_s FLOAT NOT NULL, 
	end_s FLOAT NOT NULL, 
	text TEXT NOT NULL, 
	speaker_participant_id VARCHAR, 
	speaker_source VARCHAR NOT NULL, 
	speaker_pinned BOOLEAN NOT NULL, 
	embedding BLOB, 
	created_at VARCHAR NOT NULL, 
	PRIMARY KEY (id), 
	FOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE, 
	FOREIGN KEY(chunk_id) REFERENCES meeting_audio_chunks (id)
);

CREATE INDEX idx_msegments_time ON meeting_segments (meeting_id, start_s);

CREATE TABLE meeting_segment_delivery (
	seq INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, 
	segment_id VARCHAR NOT NULL, 
	meeting_id VARCHAR NOT NULL, 
	UNIQUE (segment_id), 
	FOREIGN KEY(segment_id) REFERENCES meeting_segments (id) ON DELETE CASCADE, 
	FOREIGN KEY(meeting_id) REFERENCES meeting_sessions (id) ON DELETE CASCADE
);

CREATE TABLE sqlite_sequence(name,seq);

CREATE INDEX idx_msegment_delivery_page ON meeting_segment_delivery (meeting_id, seq);

CREATE VIRTUAL TABLE meeting_segments_fts USING fts5(
                text, content='meeting_segments', content_rowid='rowid'
            );

CREATE TABLE 'meeting_segments_fts_data'(id INTEGER PRIMARY KEY, block BLOB);

CREATE TABLE 'meeting_segments_fts_idx'(segid, term, pgno, PRIMARY KEY(segid, term)) WITHOUT ROWID;

CREATE TABLE 'meeting_segments_fts_docsize'(id INTEGER PRIMARY KEY, sz BLOB);

CREATE TABLE 'meeting_segments_fts_config'(k PRIMARY KEY, v) WITHOUT ROWID;

CREATE TRIGGER meeting_segments_fts_ai
            AFTER INSERT ON meeting_segments BEGIN
                INSERT INTO meeting_segments_fts(rowid, text)
                VALUES (new.rowid, new.text);
            END;

CREATE TRIGGER meeting_segments_fts_ad
            AFTER DELETE ON meeting_segments BEGIN
                INSERT INTO meeting_segments_fts(meeting_segments_fts, rowid, text)
                VALUES ('delete', old.rowid, old.text);
            END;

CREATE TRIGGER meeting_segments_fts_au
            AFTER UPDATE OF text ON meeting_segments BEGIN
                INSERT INTO meeting_segments_fts(meeting_segments_fts, rowid, text)
                VALUES ('delete', old.rowid, old.text);
                INSERT INTO meeting_segments_fts(rowid, text)
                VALUES (new.rowid, new.text);
            END;

CREATE TRIGGER meeting_segments_delivery_ai
                AFTER INSERT ON meeting_segments BEGIN
                    INSERT INTO meeting_segment_delivery(segment_id, meeting_id)
                    VALUES (new.id, new.meeting_id);
                END;

