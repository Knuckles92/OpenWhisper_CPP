// meeting/persist/repository.py over the app database: schema v18 tables, the chunk lifecycle,
// segments and FTS search, state write-through and cascade-safe deletion. Ports
// tests/test_meeting_repository.py and test_meeting_asr.py::test_interval_iou, and reads back a
// database OpenWhisper made (tests/fixtures/meeting_state).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "db/schema.h"
#include "meeting_state/store.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "sql/db.h"
#include "sql_support.h"

using fv::Json;
using namespace fv::meeting_state;
using namespace fv::meeting_state::test_support;

namespace {

Json chunk_fields(const std::string& meeting_id, const std::string& channel, std::int64_t seq,
                  double start_s, double duration_s, const std::string& path = "/tmp/c0.wav") {
    return {{"meeting_id", meeting_id}, {"channel", channel}, {"seq", seq},
            {"file_path", path},        {"start_s", start_s}, {"duration_s", duration_s},
            {"sample_rate", 16000}};
}

std::set<std::string> table_names(fv::Database& database) {
    std::set<std::string> names;
    for (const auto& name :
         column_values(database, "SELECT name FROM sqlite_master WHERE type = 'table'")) {
        names.insert(name.get<std::string>());
    }
    return names;
}

std::vector<Json> master_rows(fv::Database& database) {
    return database.read([](fv::sql::Connection& connection) {
        fv::sql::Statement statement = connection.prepare(
            "SELECT type, name, tbl_name, sql FROM sqlite_master ORDER BY type, name");
        std::vector<Json> rows;
        while (statement.step()) {
            Json row = Json::array();
            for (int index = 0; index < 4; ++index) {
                const auto value = statement.column(index);
                row.push_back(std::holds_alternative<std::string>(value)
                                  ? Json(std::get<std::string>(value))
                                  : Json());
            }
            rows.push_back(std::move(row));
        }
        return rows;
    });
}

} // namespace

// TestSchema

// from tests/test_meeting_repository.py::TestSchema::test_v10_tables_exist
TEST_CASE("TestSchema::test_v10_tables_exist", "[test_meeting_repository]") {
    SqlFixture f;
    const auto names = table_names(*f.database);
    for (const char* table :
         {"meeting_sessions", "meeting_audio_chunks", "meeting_segments", "meeting_participants",
          "meeting_state_items", "meeting_questions", "meeting_events"}) {
        CHECK(names.contains(table));
    }
    // Legacy names stay absent (dropped on startup).
    for (const char* table : {"meetings", "meeting_chunks", "meeting_insights"}) {
        CHECK_FALSE(names.contains(table));
    }
}

// from tests/test_meeting_repository.py::TestSchema::test_wal_mode_enabled
TEST_CASE("TestSchema::test_wal_mode_enabled", "[test_meeting_repository]") {
    SqlFixture f;
    const auto mode = column_values(*f.database, "PRAGMA journal_mode");
    REQUIRE(mode.size() == 1);
    CHECK(fv::py::lower(mode[0].get<std::string>()) == "wal");
}

// from tests/test_meeting_repository.py::TestSchema::test_migration_from_v9
TEST_CASE("TestSchema::test_migration_from_v9", "[test_meeting_repository]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "old.db";
    {
        auto connection = fv::sql::Connection::open(path, fv::sql::OpenMode::Create);
        connection.exec("CREATE TABLE schema_version (version INTEGER PRIMARY KEY)");
        connection.exec("INSERT INTO schema_version VALUES (9)");
        connection.exec(R"(
            CREATE TABLE transcription_history (
                id TEXT PRIMARY KEY, text TEXT NOT NULL, raw_text TEXT,
                timestamp TEXT NOT NULL, model TEXT NOT NULL,
                audio_file TEXT, transcription_time REAL,
                audio_duration REAL, file_size INTEGER,
                cleanup_provider TEXT, cleanup_model TEXT
            ))");
        connection.exec(
            "INSERT INTO transcription_history VALUES ('legacy-1', 'Cleaned café 日本語', "
            "'Raw café 日本語', '2025-02-03T04:05:06', 'base', 'original meeting.wav', "
            "1.25, 12.5, 4321, 'local', 'cleanup-a')");
        connection.exec(
            "INSERT INTO transcription_history VALUES ('legacy-2', 'Plain transcript', "
            "NULL, '2025-02-04T04:05:06', 'small', NULL, NULL, NULL, NULL, NULL, NULL)");
    }
    const std::string original =
        "id, text, raw_text, timestamp, model, audio_file, transcription_time, audio_duration, "
        "file_size, cleanup_provider, cleanup_model";
    for (int open = 0; open < 3; ++open) {
        fv::Database database = fv::Database::open_or_create(path);
        CHECK(column_values(database, "SELECT version FROM schema_version") ==
              std::vector<Json>{fv::schema::schema_version});
        const auto rows = database.read([&](fv::sql::Connection& connection) {
            fv::sql::Statement statement = connection.prepare(
                "SELECT " + original + " FROM transcription_history ORDER BY id");
            std::vector<std::vector<fv::sql::Value>> out;
            while (statement.step()) {
                std::vector<fv::sql::Value> row;
                for (int index = 0; index < 11; ++index) {
                    row.push_back(statement.column(index));
                }
                out.push_back(std::move(row));
            }
            return out;
        });
        REQUIRE(rows.size() == 2);
        CHECK(std::get<std::string>(rows[0][1]) == "Cleaned café 日本語");
        CHECK(std::get<std::string>(rows[0][2]) == "Raw café 日本語");
        CHECK(std::get<double>(rows[0][6]) == 1.25);
        CHECK(std::get<double>(rows[0][7]) == 12.5);
        CHECK(std::get<std::int64_t>(rows[0][8]) == 4321);
        CHECK(std::get<std::string>(rows[0][10]) == "cleanup-a");
        CHECK(std::holds_alternative<std::nullptr_t>(rows[1][2]));
        CHECK(std::get<std::string>(rows[1][4]) == "small");
        CHECK(column_values(database, "SELECT title FROM transcription_history ORDER BY id") ==
              std::vector<Json>{nullptr, nullptr});
        CHECK(table_names(database).contains("meeting_sessions"));
    }
}

// from tests/test_meeting_repository.py::TestSchema::test_agent_endpoint_json_roundtrip
TEST_CASE("TestSchema::test_agent_endpoint_json_roundtrip", "[test_meeting_repository]") {
    SqlFixture f;
    const Json endpoint = {{"profile_id", "custom_abcd1234"},
                           {"name", "LM Studio"},
                           {"kind", "custom"},
                           {"base_url", "http://127.0.0.1:1234/v1"},
                           {"api_key_env", ""}};
    f.repo->create_meeting({{"id", "m_endpoint"},
                            {"title", "Endpoint meeting"},
                            {"status", "active"},
                            {"started_at", "2026-01-01T09:00:00"},
                            {"host_token", "host-token"},
                            {"guest_token", "guest-token"},
                            {"cloud_enabled", true},
                            {"spool_dir", "/tmp/spool"},
                            {"agent_provider", "custom_abcd1234"},
                            {"agent_model", "local-qwen"},
                            {"agent_endpoint_json", fv::py::dumps(endpoint)}});
    const Json meeting = *f.repo->get_meeting("m_endpoint");
    const Json stored = fv::py::loads(meeting["agent_endpoint_json"].get<std::string>());
    CHECK(stored["base_url"] == "http://127.0.0.1:1234/v1");
    CHECK(stored["profile_id"] == "custom_abcd1234");
}

// TestMeetingLifecycle

// from tests/test_meeting_repository.py::TestMeetingLifecycle::test_create_get_list_update
TEST_CASE("TestMeetingLifecycle::test_create_get_list_update", "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo);
    Json meeting = *f.repo->get_meeting("m_test1");
    CHECK(meeting["title"] == "Test meeting");
    CHECK(meeting["status"] == "active");

    f.repo->update_meeting("m_test1", {{"status", "ended"}, {"title", "Renamed"}});
    meeting = *f.repo->get_meeting("m_test1");
    CHECK(meeting["status"] == "ended");
    CHECK(meeting["title"] == "Renamed");

    CHECK(ids_of(f.repo->list_meetings()) == std::vector<std::string>{"m_test1"});
}

// from tests/test_meeting_repository.py::TestMeetingLifecycle::test_heartbeat_and_interrupted
TEST_CASE("TestMeetingLifecycle::test_heartbeat_and_interrupted", "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo);
    f.repo->heartbeat("m_test1", 4242, "2026-01-01T00:00:05Z");
    const Json meeting = *f.repo->get_meeting("m_test1");
    CHECK(meeting["app_pid"] == 4242);
    CHECK(meeting["app_heartbeat_at"] == "2026-01-01T00:00:05Z");

    CHECK(ids_of(f.repo->find_interrupted_meetings()) == std::vector<std::string>{"m_test1"});
    f.repo->update_meeting("m_test1", {{"status", "ended"}});
    CHECK(f.repo->find_interrupted_meetings().empty());
}

// from
// tests/test_meeting_repository.py::TestMeetingLifecycle::test_terminal_meeting_with_unfinished_audio_requires_recovery
TEST_CASE("TestMeetingLifecycle::test_terminal_meeting_with_unfinished_audio_requires_recovery",
          "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo);
    const auto chunk_id = f.repo->register_chunk(chunk_fields("m_test1", "mic", 0, 0.0, 20.0));
    f.repo->update_meeting("m_test1", {{"status", "ended"}});
    CHECK(ids_of(f.repo->find_interrupted_meetings()) == std::vector<std::string>{"m_test1"});
    f.repo->set_chunk_status(chunk_id, "done", std::nullopt);
    CHECK(f.repo->find_interrupted_meetings().empty());
}

// from
// tests/test_meeting_repository.py::TestMeetingLifecycle::test_rename_updates_canonical_row_and_snapshot
TEST_CASE("TestMeetingLifecycle::test_rename_updates_canonical_row_and_snapshot",
          "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo);
    f.repo->persist_state(
        "m_test1", {{"meeting_id", "m_test1"}, {"seq", 2}, {"title", "Old"}, {"status", "ended"}});
    f.repo->rename_meeting("m_test1", "New title");
    const Json meeting = *f.repo->get_meeting("m_test1");
    CHECK(meeting["title"] == "New title");
    CHECK(fv::py::loads(meeting["state_json"].get<std::string>())["title"] == "New title");
}

// from
// tests/test_meeting_repository.py::TestMeetingLifecycle::test_past_meeting_summary_page_is_bounded_searchable_and_aggregated
TEST_CASE(
    "TestMeetingLifecycle::test_past_meeting_summary_page_is_bounded_searchable_and_aggregated",
    "[test_meeting_repository]") {
    SqlFixture f;
    for (int index = 0; index < 4; ++index) {
        f.repo->create_meeting(
            {{"id", "m_page_" + std::to_string(index)},
             {"title", "Review " + std::to_string(index)},
             {"status", index < 3 ? "ended" : "active"},
             {"started_at", "2026-08-0" + std::to_string(index + 1) + "T09:00:00+00:00"},
             {"host_token", "host-token"},
             {"guest_token", "guest-token"},
             {"cloud_enabled", false},
             {"spool_dir", "/tmp/spool"}});
    }
    const auto chunk_id =
        f.repo->register_chunk(chunk_fields("m_page_2", "loopback", 0, 0.0, 4.0, "/tmp/page.wav"));
    TranscriptSegment segment =
        make_segment("m_page_2", "sg_page", 1.0, 3.0, "quarterly capacity planning", "loopback");
    segment.chunk_id = chunk_id;
    f.repo->add_segments({segment});

    PastMeetingQuery query;
    query.limit = 2;
    const auto page = f.repo->list_past_meeting_summaries(query);

    CHECK(ids_of(page) == std::vector<std::string>{"m_page_2", "m_page_1"});
    const Json& summary = page[0]["content_summary"];
    CHECK(summary["audio_chunks"] == 1);
    CHECK(summary["transcript_segments"] == 1);
    CHECK(summary["can_rerun_speakers"] == true);
    CHECK(summary["preview_text"] == "quarterly capacity planning");
    CHECK(f.repo->last_summary_query_count() == 4);
    query.query = "capacity";
    CHECK(ids_of(f.repo->list_past_meeting_summaries(query)) ==
          std::vector<std::string>{"m_page_2"});
}

// TestChunks

// from
// tests/test_meeting_repository.py::TestChunks::test_recovery_registration_is_idempotent_by_channel_sequence
TEST_CASE("TestChunks::test_recovery_registration_is_idempotent_by_channel_sequence",
          "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo);
    Json fields = chunk_fields("m_test1", "mic", 4, 8.0, 2.0, "/tmp/recovered.wav");
    fields["asr_status"] = "pending";
    const auto first = f.repo->register_chunk_if_missing(fields);
    fields["file_path"] = "/tmp/duplicate.wav";
    const auto second = f.repo->register_chunk_if_missing(fields);
    CHECK(first.created == true);
    CHECK(second.created == false);
    CHECK(second.id == first.id);
    CHECK(f.repo->get_audio_chunks("m_test1").size() == 1);
}

// from tests/test_meeting_repository.py::TestChunks::test_chunk_lifecycle_and_retry_visibility
TEST_CASE("TestChunks::test_chunk_lifecycle_and_retry_visibility", "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo);
    const auto chunk_id = f.repo->register_chunk(chunk_fields("m_test1", "mic", 0, 0.0, 20.0));
    CHECK(f.repo->get_pending_chunks("m_test1", std::nullopt, {}).size() == 1);

    f.repo->set_chunk_status(chunk_id, "processing", std::nullopt);
    f.repo->set_chunk_status(chunk_id, "failed", "boom");
    const auto pending = f.repo->get_pending_chunks("m_test1", std::nullopt, {});
    REQUIRE(pending.size() == 1); // failed with attempts < 3 stays retryable
    CHECK(pending[0]["asr_attempts"] == 1);
    CHECK(pending[0]["asr_error"] == "boom");

    for (int i = 0; i < 2; ++i) {
        f.repo->set_chunk_status(chunk_id, "processing", std::nullopt);
        f.repo->set_chunk_status(chunk_id, "failed", "boom");
    }
    CHECK(f.repo->get_pending_chunks("m_test1", std::nullopt, {}).empty()); // exhausted

    f.repo->set_chunk_status(chunk_id, "done", std::nullopt);
    CHECK(f.repo->get_pending_chunks("m_test1", std::nullopt, {}).empty());
}

// from
// tests/test_meeting_repository.py::TestChunks::test_bounded_pending_page_and_recovery_preserve_retry_budget
TEST_CASE("TestChunks::test_bounded_pending_page_and_recovery_preserve_retry_budget",
          "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo);
    std::vector<std::int64_t> chunks;
    for (int index = 0; index < 4; ++index) {
        chunks.push_back(
            f.repo->register_chunk(chunk_fields("m_test1", "mic", index, static_cast<double>(index),
                                                1.0, "/tmp/c" + std::to_string(index) + ".wav")));
    }
    f.repo->set_chunk_status(chunks[1], "processing", std::nullopt);
    f.repo->set_chunk_status(chunks[1], "failed", "transient");
    f.repo->set_chunk_status(chunks[2], "processing", std::nullopt);
    f.repo->set_chunk_status(chunks[2], "blocked", "Saved WAV is missing");
    for (int i = 0; i < 3; ++i) {
        f.repo->set_chunk_status(chunks[3], "processing", std::nullopt);
        f.repo->set_chunk_status(chunks[3], "failed", "backend failed");
    }

    const auto page = f.repo->get_pending_chunks("m_test1", 1, {chunks[0]});
    REQUIRE(page.size() == 1);
    CHECK(page[0]["id"] == chunks[1]);
    CHECK(f.repo->reset_unfinished_chunks("m_test1", false) == 2);
    std::map<std::int64_t, Json> rows;
    for (const auto& row : f.repo->get_audio_chunks("m_test1")) {
        rows[row["id"].get<std::int64_t>()] = row;
    }
    CHECK(rows[chunks[1]]["asr_attempts"] == 1);
    CHECK(rows[chunks[2]]["asr_status"] == "blocked");
    CHECK(rows[chunks[3]]["asr_attempts"] == 3);
    const auto pending_ids = [&] {
        std::set<std::int64_t> ids;
        for (const auto& row : f.repo->get_pending_chunks("m_test1", std::nullopt, {})) {
            ids.insert(row["id"].get<std::int64_t>());
        }
        return ids;
    };
    CHECK(pending_ids() == std::set<std::int64_t>{chunks[0], chunks[1]});

    CHECK(f.repo->reset_unfinished_chunks("m_test1", true) == 4);
    CHECK(pending_ids() == std::set<std::int64_t>(chunks.begin(), chunks.end()));
    for (const auto& row : f.repo->get_audio_chunks("m_test1")) {
        CHECK(row["asr_attempts"] == 0);
    }

    f.repo->set_chunk_status(chunks[0], "processing", std::nullopt);
    f.repo->defer_chunk_after_connection_failure(chunks[0], "host offline");
    for (const auto& row : f.repo->get_audio_chunks("m_test1")) {
        if (row["id"] == chunks[0]) {
            CHECK(row["asr_status"] == "pending");
            CHECK(row["asr_attempts"] == 0);
            CHECK(row["asr_error"] == "host offline");
        }
    }
}

// from
// tests/test_meeting_repository.py::TestChunks::test_atomic_commit_is_idempotent_and_done_cannot_regress
TEST_CASE("TestChunks::test_atomic_commit_is_idempotent_and_done_cannot_regress",
          "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo);
    const auto chunk_id = f.repo->register_chunk(chunk_fields("m_test1", "mic", 0, 0.0, 20.0));
    TranscriptSegment segment = make_segment("m_test1", "sg_commit", 0.0, 2.0, "durable");
    segment.chunk_id = chunk_id;
    auto commit = f.repo->commit_chunk_transcription("m_test1", chunk_id, {segment});
    CHECK(commit.committed == true);
    CHECK(ids_of(commit.rows) == std::vector<std::string>{"sg_commit"});

    TranscriptSegment retry = make_segment("m_test1", "sg_other", 2.0, 4.0, "duplicate retry");
    retry.chunk_id = chunk_id;
    commit = f.repo->commit_chunk_transcription("m_test1", chunk_id, {retry});
    CHECK(commit.committed == false);
    CHECK(ids_of(commit.rows) == std::vector<std::string>{"sg_commit"});
    f.repo->set_chunk_status(chunk_id, "failed", "late callback");
    CHECK(f.repo->count_unfinished_chunks("m_test1") == 0);
}

// TestSegments

// from tests/test_meeting_repository.py::TestSegments::test_add_query_and_speaker_update
TEST_CASE("TestSegments::test_add_query_and_speaker_update", "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo);
    f.repo->add_segments({make_segment("m_test1", "sg_1", 0.0, 2.0, "first"),
                          make_segment("m_test1", "sg_2", 2.0, 4.0, "second"),
                          make_segment("m_test1", "sg_3", 4.0, 6.0, "third")});
    CHECK(f.repo->segment_exists("m_test1", "sg_2"));
    CHECK_FALSE(f.repo->segment_exists("m_test1", "sg_nope"));

    CHECK(ids_of(f.repo->get_segments("m_test1")) ==
          std::vector<std::string>{"sg_1", "sg_2", "sg_3"});
    CHECK(ids_of(f.repo->get_segments("m_test1", 1.0)) == std::vector<std::string>{"sg_2", "sg_3"});
    CHECK(ids_of(f.repo->get_last_segments("m_test1", 2)) ==
          std::vector<std::string>{"sg_2", "sg_3"});

    f.repo->update_segment_speaker("m_test1", "sg_1", std::nullopt, "human", true);
    const Json segment = *f.repo->get_segment("m_test1", "sg_1");
    CHECK(segment["speaker_participant_id"].is_null());
    CHECK(segment["speaker_pinned"] == true);
}

// from
// tests/test_meeting_repository.py::TestSegments::test_unreadable_state_does_not_block_transcript_reads
TEST_CASE("TestSegments::test_unreadable_state_does_not_block_transcript_reads",
          "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo);
    f.repo->add_segments({make_segment("m_test1", "sg_1", 0.0, 2.0, "first")});
    f.repo->update_meeting("m_test1", {{"state_json", "{not json"}});
    const auto rows = f.repo->get_segments("m_test1");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["text"] == "first");
}

// from tests/test_meeting_repository.py::TestSegments::test_keyset_paging_handles_equal_timestamps
TEST_CASE("TestSegments::test_keyset_paging_handles_equal_timestamps",
          "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo);
    std::vector<TranscriptSegment> segments;
    for (int i = 0; i < 5; ++i) {
        segments.push_back(
            make_segment("m_test1", "sg_" + std::to_string(i), 5.0, 6.0, std::to_string(i)));
    }
    f.repo->add_segments(segments);
    const auto first = f.repo->get_segments_page("m_test1", std::nullopt, std::nullopt, 2);
    const auto second = f.repo->get_segments_page("m_test1", first.back()["start_s"].get<double>(),
                                                  first.back()["id"].get<std::string>(), 2);
    const auto third = f.repo->get_segments_page("m_test1", second.back()["start_s"].get<double>(),
                                                 second.back()["id"].get<std::string>(), 2);
    std::vector<std::string> ids = ids_of(first);
    for (const auto& page : {second, third}) {
        for (const auto& id : ids_of(page)) {
            ids.push_back(id);
        }
    }
    CHECK(ids == std::vector<std::string>{"sg_0", "sg_1", "sg_2", "sg_3", "sg_4"});
}

// from
// tests/test_meeting_repository.py::TestSegments::test_segment_reads_and_updates_are_meeting_scoped
TEST_CASE("TestSegments::test_segment_reads_and_updates_are_meeting_scoped",
          "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo, "m_one");
    make_meeting(*f.repo, "m_two");
    f.repo->add_segments({make_segment("m_one", "sg_one")});
    CHECK_FALSE(f.repo->get_segment("m_two", "sg_one").has_value());
    CHECK_FALSE(f.repo->segment_exists("m_two", "sg_one"));
    f.repo->update_segment_speaker("m_two", "sg_one", std::nullopt, "human", true);
    CHECK(f.repo->get_segment("m_one", "sg_one")->at("speaker_pinned") == false);
}

// TestSearchAndDelete

// from
// tests/test_meeting_repository.py::TestSearchAndDelete::test_fts_search_and_orphan_free_delete
TEST_CASE("TestSearchAndDelete::test_fts_search_and_orphan_free_delete",
          "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo);
    f.repo->add_segments(
        {make_segment("m_test1", "sg_q", 0.0, 2.0, "the quarterly budget review")});
    const auto hits = f.repo->search_transcripts("budget", std::nullopt);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]["segment_id"] == "sg_q");
    CHECK(hits[0]["title"] == "Test meeting");
    CHECK(hits[0]["snippet"] == "the quarterly [budget] review");

    // FTS operators from user input must not break the query.
    CHECK(f.repo->search_transcripts("budget\" OR 1=1 NEAR(", std::nullopt).empty());

    bool counted = false;
    f.repo->set_feature_counter([&] { counted = true; });
    f.repo->delete_meeting("m_test1");
    CHECK(counted);
    CHECK_FALSE(f.repo->get_meeting("m_test1").has_value());
    CHECK(f.repo->get_segments("m_test1").empty());
    // The FTS index must not serve orphaned rows after deletion.
    CHECK(f.repo->search_transcripts("budget", std::nullopt).empty());
}

// from
// tests/test_meeting_repository.py::TestSearchAndDelete::test_fts_search_excludes_meeting_and_honors_limit
TEST_CASE("TestSearchAndDelete::test_fts_search_excludes_meeting_and_honors_limit",
          "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo, "m_live");
    make_meeting(*f.repo, "m_past");
    f.repo->add_segments(
        {make_segment("m_live", "sg_live", 0.0, 2.0, "the quarterly budget review"),
         make_segment("m_past", "sg_past_a", 0.0, 2.0, "budget planning for next quarter"),
         make_segment("m_past", "sg_past_b", 2.0, 4.0, "another budget follow-up")});
    const auto hits = f.repo->search_transcripts("budget", "m_live", 1);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]["meeting_id"] == "m_past");
    const auto more = f.repo->search_transcripts("budget", "m_live");
    CHECK(more.size() == 2);
    for (const auto& hit : more) {
        CHECK(hit["meeting_id"] == "m_past");
    }
}

// TestWriteThrough

// from tests/test_meeting_repository.py::TestWriteThrough::test_ops_applied_mirrors_and_audit
TEST_CASE("TestWriteThrough::test_ops_applied_mirrors_and_audit", "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo);
    f.repo->add_segments({make_segment("m_test1", "sg_evidence", 1.0, 3.0, "supporting evidence")});
    SegmentPredicates predicates;
    auto repo = f.repo;
    predicates.segment_exists = [repo](std::string_view id) {
        return repo->segment_exists("m_test1", id);
    };
    MeetingStateStore store(state_for("m_test1"), f.repo, {}, f.clock, f.ids, predicates);

    const auto added = store.apply("agent", "agent", Json::parse(R"([{"op": "add_item",
        "card": "decisions", "text": "Ship it", "evidence": ["sg_evidence"]}])"))[0];
    REQUIRE(added.ok);

    CHECK(column_values(*f.database, "SELECT text FROM meeting_state_items") ==
          std::vector<Json>{"Ship it"});
    CHECK(column_values(*f.database, "SELECT card FROM meeting_state_items") ==
          std::vector<Json>{"decisions"});
    CHECK(column_values(*f.database, "SELECT seq FROM meeting_events") ==
          std::vector<Json>{*added.seq});
    CHECK(column_values(*f.database, "SELECT actor_type FROM meeting_events") ==
          std::vector<Json>{"agent"});

    const Json event = *f.repo->get_event("m_test1", *added.seq);
    CHECK(event["inverse"] == Json({{"op", "remove_item"}, {"id", *added.target_id}}));

    const auto listed = f.repo->list_events("m_test1");
    const auto original = std::find_if(listed.begin(), listed.end(),
                                       [&](const Json& e) { return e["seq"] == *added.seq; });
    REQUIRE(original != listed.end());
    CHECK((*original)["payload"]["text"] == "Ship it");

    const Json meeting = *f.repo->get_meeting("m_test1");
    CHECK(fv::py::loads(meeting["state_json"].get<std::string>())["seq"] == *added.seq);
    CHECK(meeting["state_seq"] == *added.seq);

    const auto undone = store.undo(*added.seq, "p_host");
    REQUIRE(undone.size() == 1);
    CHECK(undone[0].ok);
    CHECK(store.undo(*added.seq, "p_host").empty());
    const auto events = f.repo->list_events("m_test1");
    for (const auto& e : events) {
        if (e["seq"] == *added.seq) {
            CHECK(e["undoable"] == false);
        }
        if (e["seq"] == *undone[0].seq) {
            CHECK(e["undoable"] == true);
        }
    }
}

// from tests/test_meeting_repository.py::TestWriteThrough::test_question_and_participant_mirrors
TEST_CASE("TestWriteThrough::test_question_and_participant_mirrors", "[test_meeting_repository]") {
    SqlFixture f;
    make_meeting(*f.repo);
    MeetingStateStore store(state_for("m_test1"), f.repo, {}, f.clock, f.ids);
    store.apply("agent", "agent", Json::parse(R"([
        {"op": "ask_question", "text": "Deadline confirmed?", "evidence": ["sg_anchor"]},
        {"op": "upsert_participant", "display_name": "Sam", "evidence": ["sg_anchor"]}])"));
    CHECK(column_values(*f.database, "SELECT count(*) FROM meeting_questions") ==
          std::vector<Json>{1});
    CHECK(column_values(*f.database, "SELECT display_name FROM meeting_participants") ==
          std::vector<Json>{"Sam"});
    CHECK(column_values(*f.database, "SELECT name_source FROM meeting_participants") ==
          std::vector<Json>{"agent_inferred"});
}

// TestReplaceFinalTranscript

// from
// tests/test_meeting_repository.py::TestReplaceFinalTranscript::test_keeps_pinned_speaker_and_remaps_evidence
TEST_CASE("TestReplaceFinalTranscript::test_keeps_pinned_speaker_and_remaps_evidence",
          "[test_meeting_repository]") {
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    std::vector<TranscriptSegment> live = {
        make_segment(meeting_id, "sg_old1", 0.0, 2.0, "draft one"),
        make_segment(meeting_id, "sg_old2", 2.0, 4.0, "draft two")};
    live[0].speaker_pinned = true;
    live[0].speaker_participant_id = "p_me";
    live[0].speaker_source = "human";
    f.repo->add_segments(live);
    f.repo->update_meeting(meeting_id, {{"state_json", fv::py::dumps(Json::parse(R"({
        "meeting_id": "m_test1",
        "cards": {
            "key_points": [
                {"id": "it_human", "text": "kept", "status": "edited", "pinned": false, "evidence": ["sg_old1"]},
                {"id": "it_agent", "text": "stale", "status": "proposed", "evidence": ["sg_old2"]}],
            "user_notes": [], "decisions": [], "action_items": [], "risks": [], "timeline": []},
        "questions": [],
        "rolling_summary_evidence": ["sg_old2"]})"))}});
    std::vector<TranscriptSegment> cleaned = {
        make_segment(meeting_id, "sg_new1", 0.1, 2.1, "clean one"),
        make_segment(meeting_id, "sg_new2", 2.1, 4.1, "clean two")};

    const auto replaced = f.repo->replace_final_transcript(meeting_id, cleaned);

    const auto ids = ids_of(replaced.rows);
    CHECK(std::find(ids.begin(), ids.end(), "sg_new1") != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "sg_new2") != ids.end());
    CHECK(std::find(replaced.deleted_ids.begin(), replaced.deleted_ids.end(), "sg_old2") !=
          replaced.deleted_ids.end());
    const auto mapped = std::find_if(replaced.id_map.begin(), replaced.id_map.end(),
                                     [](const auto& entry) { return entry.first == "sg_old1"; });
    REQUIRE(mapped != replaced.id_map.end());
    CHECK(mapped->second == "sg_new1");
    for (const auto& row : replaced.rows) {
        if (row["id"] == "sg_new1") {
            CHECK(row["speaker_pinned"] == true);
            CHECK(row["speaker_participant_id"] == "p_me");
        }
    }
    const Json state =
        fv::py::loads((*f.repo->get_meeting(meeting_id))["state_json"].get<std::string>());
    CHECK(state["cards"]["key_points"][0]["evidence"] == Json::array({"sg_new1"}));
    // Proposed agent items are remapped too, so grounded live content can survive a re-decode
    // for the final consolidation to reconcile.
    CHECK(state["cards"]["key_points"][1]["evidence"] == Json::array({"sg_new2"}));
    const auto remaining = ids_of(f.repo->get_segments(meeting_id));
    CHECK(std::set<std::string>(remaining.begin(), remaining.end()) ==
          std::set<std::string>{"sg_new1", "sg_new2"});
}

// from
// tests/test_meeting_repository.py::TestReplaceFinalTranscript::test_offline_speaker_labels_win_over_unpinned_live_labels
TEST_CASE("TestReplaceFinalTranscript::test_offline_speaker_labels_win_over_unpinned_live_labels",
          "[test_meeting_repository]") {
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    std::vector<TranscriptSegment> live = {make_segment(meeting_id, "sg_live1", 0.0, 2.0, "a"),
                                           make_segment(meeting_id, "sg_live2", 2.0, 4.0, "b")};
    for (auto& segment : live) {
        segment.channel = "loopback";
        segment.speaker_participant_id = "p_live";
        segment.speaker_source = "diarizer";
    }
    f.repo->add_segments(live);
    TranscriptSegment off1 = make_segment(meeting_id, "sg_off1", 0.0, 2.0, "a", "loopback");
    off1.speaker_participant_id = "p_offline";
    off1.speaker_source = "diarizer";
    const TranscriptSegment off2 = make_segment(meeting_id, "sg_off2", 2.0, 4.0, "b", "loopback");

    const auto replaced = f.repo->replace_final_transcript(meeting_id, {off1, off2});

    std::map<std::string, Json> by_id;
    for (const auto& row : replaced.rows) {
        by_id[row["id"].get<std::string>()] = row;
    }
    CHECK(by_id["sg_off1"]["speaker_participant_id"] == "p_offline");
    // An unlabeled offline row keeps the live label rather than none.
    CHECK(by_id["sg_off2"]["speaker_participant_id"] == "p_live");
}

// from
// tests/test_meeting_repository.py::TestReplaceFinalTranscript::test_evidence_on_retained_pinned_rows_is_kept
TEST_CASE("TestReplaceFinalTranscript::test_evidence_on_retained_pinned_rows_is_kept",
          "[test_meeting_repository]") {
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    TranscriptSegment pinned = make_segment(meeting_id, "sg_pin", 50.0, 52.0, "kept");
    pinned.speaker_pinned = true;
    pinned.speaker_participant_id = "p_me";
    pinned.speaker_source = "human";
    f.repo->add_segments({pinned});
    f.repo->update_meeting(meeting_id, {{"state_json", fv::py::dumps(Json::parse(R"({
        "meeting_id": "m_test1",
        "cards": {
            "key_points": [{"id": "it_1", "text": "cites pinned", "status": "proposed", "evidence": ["sg_pin"]}],
            "user_notes": [], "decisions": [], "action_items": [], "risks": [], "timeline": []},
        "questions": []})"))}});

    const auto replaced = f.repo->replace_final_transcript(
        meeting_id, {make_segment(meeting_id, "sg_new", 0.0, 2.0, "elsewhere")});

    CHECK(std::find(replaced.deleted_ids.begin(), replaced.deleted_ids.end(), "sg_pin") ==
          replaced.deleted_ids.end());
    const Json state =
        fv::py::loads((*f.repo->get_meeting(meeting_id))["state_json"].get<std::string>());
    CHECK(state["cards"]["key_points"][0]["evidence"] == Json::array({"sg_pin"}));
}

// from
// tests/test_meeting_repository.py::TestReplaceFinalTranscript::test_segment_flags_reports_existing_ids_and_pins
TEST_CASE("TestReplaceFinalTranscript::test_segment_flags_reports_existing_ids_and_pins",
          "[test_meeting_repository]") {
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    TranscriptSegment plain = make_segment(meeting_id, "sg_plain", 0.0, 1.0, "x");
    TranscriptSegment pinned = make_segment(meeting_id, "sg_pinned", 1.0, 2.0, "y");
    pinned.speaker_pinned = true;
    f.repo->add_segments({plain, pinned});

    const auto flags = f.repo->segment_flags(meeting_id, {"sg_plain", "sg_pinned", "sg_missing"});

    CHECK(flags ==
          std::map<std::string, bool, std::less<>>{{"sg_plain", false}, {"sg_pinned", true}});
}

// from tests/test_meeting_repository.py::TestReplaceFinalTranscript::test_mark_chunks_done
TEST_CASE("TestReplaceFinalTranscript::test_mark_chunks_done", "[test_meeting_repository]") {
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    Json fields = chunk_fields(meeting_id, "mic", 0, 0.0, 1.0, "/tmp/a.wav");
    fields["asr_status"] = "pending";
    const auto chunk_id = f.repo->register_chunk(fields);
    CHECK(f.repo->mark_chunks_done(meeting_id) == 1);
    const auto chunks = f.repo->get_audio_chunks(meeting_id);
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0]["id"] == chunk_id);
    CHECK(chunks[0]["asr_status"] == "done");
}

// from tests/test_meeting_asr.py::test_interval_iou
TEST_CASE("test_interval_iou", "[test_meeting_asr]") {
    CHECK(interval_iou(0, 10, 5, 15) == Catch::Approx(5.0 / 15.0));
    CHECK(interval_iou(0, 5, 5, 10) == 0.0);
    CHECK(interval_iou(2, 2, 2, 2) == 0.0);
}

// Beyond OpenWhisper's tests: the acceptance of the package.

TEST_CASE("a database OpenWhisper made reads back every meeting, segment and event unchanged",
          "[meeting_state_store]") {
    const auto fixtures = fv::testkit::source_root() / "tests" / "fixtures" / "meeting_state";
    const Json expected = fv::py::loads([&] {
        std::ifstream in(fixtures / "openwhisper-meetings.json", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }());
    const fv::testkit::TempDataRoot root;
    std::filesystem::copy_file(fixtures / "openwhisper-meetings.db", root.path() / "copy.db");
    fv::Database database = fv::Database::open_or_create(root.path() / "copy.db");
    SqlMeetingRepository repo(database);
    const std::vector<Json> before = master_rows(database);

    for (const auto& [meeting_id, row] : expected["meetings"].items()) {
        INFO(meeting_id);
        CHECK(fv::py::py_equal(*repo.get_meeting(meeting_id), row));
        CHECK(fv::py::py_equal(Json(repo.get_audio_chunks(meeting_id)),
                               expected["chunks"][meeting_id]));
        CHECK(fv::py::py_equal(Json(repo.get_segments(meeting_id)),
                               expected["segments"][meeting_id]));
        CHECK(fv::py::py_equal(Json(repo.list_events(meeting_id, std::nullopt, 500)),
                               expected["events"][meeting_id]));
        Json details = Json::array();
        for (const auto& event : repo.list_events(meeting_id, std::nullopt, 500)) {
            details.push_back(*repo.get_event(meeting_id, event["seq"].get<std::int64_t>()));
        }
        CHECK(fv::py::py_equal(details, expected["event_detail"][meeting_id]));
        CHECK(fv::py::py_equal(Json(repo.get_segments_delivery_page(meeting_id)),
                               expected["delivery"][meeting_id]));
    }
    Json listed = Json::array();
    for (const auto& row : repo.list_meetings()) {
        listed.push_back(row["id"]);
    }
    CHECK(listed == expected["list_meetings"]);
    CHECK(repo.get_delivery_cursor("m_fixture_ended", "cards") == expected["cursors"]["cards"]);
    CHECK(repo.get_delivery_cursor("m_fixture_ended", "notes") == expected["cursors"]["notes"]);
    CHECK(fv::py::py_equal(Json(repo.list_past_meeting_summaries()), expected["past_page"]));
    PastMeetingQuery local;
    local.origin = PastMeetingQuery::Origin::Local;
    CHECK(fv::py::py_equal(Json(repo.list_past_meeting_summaries(local)),
                           expected["past_page_local"]));
    PastMeetingQuery query;
    query.query = "anthropic";
    Json query_ids = Json::array();
    for (const auto& row : repo.list_past_meeting_summaries(query)) {
        query_ids.push_back(row["id"]);
    }
    CHECK(query_ids == expected["past_page_query"]);
    Json times = Json::array();
    for (const auto& time : repo.list_past_meeting_times()) {
        times.push_back(
            Json::array({time.id, time.started_at, time.ended_at ? Json(*time.ended_at) : Json(),
                         time.paused_total_s,
                         time.origin_device_name ? Json(*time.origin_device_name) : Json()}));
    }
    CHECK(fv::py::py_equal(times, expected["past_times"]));
    std::set<std::string> interrupted;
    for (const auto& row : repo.find_interrupted_meetings()) {
        interrupted.insert(row["id"].get<std::string>());
    }
    CHECK(Json(interrupted) == expected["interrupted"]);
    CHECK(fv::py::py_equal(Json(repo.search_transcripts("budget", std::nullopt)),
                           expected["search"]));
    CHECK(fv::py::py_equal(repo.get_meeting_content_summary("m_fixture_interrupted"),
                           expected["content_summary"]));
    // fv_db owns the schema: reading and writing through the repository changes none of it.
    repo.add_segments({make_segment("m_fixture_ended", "sg_new", 30.0, 31.0, "late words")});
    repo.delete_meeting("m_fixture_paired");
    CHECK(master_rows(database) == before);
}

TEST_CASE("a v17 database migrates and reads back through the repository",
          "[meeting_state_store]") {
    const fv::testkit::TempDataRoot root;
    std::filesystem::copy_file(fv::testkit::source_root() / "tests" / "fixtures" / "db" /
                                   "schema_v17.db",
                               root.path() / "v17.db");
    fv::Database database = fv::Database::open_or_create(root.path() / "v17.db");
    SqlMeetingRepository repo(database);
    const Json meeting = *repo.get_meeting("meet-1");
    CHECK(meeting["title"] == "Standup");
    CHECK(meeting["status"] == "ended");
    CHECK(ids_of(repo.get_segments("meet-1")) == std::vector<std::string>{"meet-1-0", "meet-1-1"});
    // Delivery was backfilled once, in row order.
    CHECK(ids_of(repo.get_segments_delivery_page("meet-1")) ==
          std::vector<std::string>{"meet-1-1", "meet-1-0"});
    const auto hits = repo.search_transcripts("words", std::nullopt);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]["segment_id"] == "meet-1-1");
    CHECK(repo.list_past_meeting_summaries()[0]["content_summary"]["preview_text"] ==
          "hello there");
}

TEST_CASE("the past-meeting calendar feed and origin filters", "[meeting_state_store]") {
    SqlFixture f;
    const auto add = [&](const std::string& id, const char* status, const char* started,
                         const Json& extra) {
        Json fields = {
            {"id", id},          {"title", id},        {"status", status}, {"started_at", started},
            {"host_token", "h"}, {"guest_token", "g"}, {"spool_dir", "s"}};
        for (const auto& [key, value] : extra.items()) {
            fields[key] = value;
        }
        f.repo->create_meeting(fields);
    };
    add("m_local", "ended", "2026-03-01T10:00:00Z",
        {{"ended_at", "2026-03-01T10:30:00Z"}, {"paused_total_s", 5}});
    add("m_paired", "failed", "2026-03-02T10:00:00Z",
        {{"origin_device_id", "dev-1"}, {"origin_device_name", "Laptop"}});
    add("m_live", "paused", "2026-03-03T10:00:00Z", Json::object());

    const auto times = f.repo->list_past_meeting_times();
    REQUIRE(times.size() == 2);
    CHECK(times[0] == PastMeetingTime{"m_local", "2026-03-01T10:00:00Z", "2026-03-01T10:30:00Z",
                                      5.0, std::nullopt});
    CHECK(times[1] ==
          PastMeetingTime{"m_paired", "2026-03-02T10:00:00Z", std::nullopt, 0.0, "Laptop"});

    PastMeetingQuery query;
    query.origin = PastMeetingQuery::Origin::Device;
    query.origin_device_id = "dev-1";
    CHECK(ids_of(f.repo->list_past_meeting_summaries(query)) ==
          std::vector<std::string>{"m_paired"});
    query.origin = PastMeetingQuery::Origin::Local;
    CHECK(ids_of(f.repo->list_past_meeting_summaries(query)) ==
          std::vector<std::string>{"m_local"});
    query = {};
    query.include_running = true;
    CHECK(ids_of(f.repo->list_past_meeting_summaries(query)) ==
          std::vector<std::string>{"m_live", "m_paired", "m_local"});
    query.cursor_started_at = "2026-03-03T10:00:00Z";
    query.cursor_id = "m_live";
    CHECK(ids_of(f.repo->list_past_meeting_summaries(query)) ==
          std::vector<std::string>{"m_paired", "m_local"});
    query = {};
    query.started_after = "2026-03-02";
    CHECK(ids_of(f.repo->list_past_meeting_summaries(query)) ==
          std::vector<std::string>{"m_paired"});
    query = {};
    query.started_from = "2026-03-01";
    query.started_before = "2026-03-02";
    CHECK(ids_of(f.repo->list_past_meeting_summaries(query)) ==
          std::vector<std::string>{"m_local"});
    CHECK(f.repo->get_meeting_content_summary("m_nowhere") ==
          Json::parse(R"({"has_audio": false, "has_loopback_audio": false,
              "has_transcript": false, "is_empty": true, "audio_chunks": 0,
              "transcript_segments": 0, "can_rerun_speakers": false, "preview_text": ""})"));
    CHECK(f.repo->last_summary_query_count() == 1);
}
