// Staging and applying a restore: the restore half of OpenWhisper's tests/test_backup.py and
// tests/test_personalize_foundation_backup.py, and the restore of an OpenWhisper .owbackup.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "archive/zip_reader.h"
#include "archive/zip_writer.h"
#include "backup/backup.h"
#include "backup/create.h"
#include "backup/names.h"
#include "backup/restore.h"
#include "backup/testing/restore_faults.h"
#include "backup/testing/sample_data.h"
#include "base/hash.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "db/database.h"
#include "pyport/json.h"
#include "sql/db.h"
#include "testkit/testkit.h"

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Message;
using fv::Json;
using fv::archive::ZipReader;
using fv::archive::ZipWriter;
using fv::backup::apply_pending_restore;
using fv::backup::BackupError;
using fv::backup::create_backup;
using fv::backup::CreateOptions;
using fv::backup::pending_restore_exists;
using fv::backup::prepare_restore;
using fv::backup::previous_data_dir;
using fv::backup::testing::make_sample_data;
using fv::backup::testing::read_bytes;
using fv::backup::testing::RestoreFaults;
using fv::backup::testing::ScopedRestoreFaults;
using fv::backup::testing::SimulatedCrash;
using fv::backup::testing::write_bytes;

namespace {

const fv::backup::Names& names() {
    return fv::backup::names_for(fv::identity());
}

fs::path settings_of(const fs::path& root) {
    return root / fv::path_from_utf8(names().settings_file);
}

fs::path database_of(const fs::path& root) {
    return root / fv::path_from_utf8(names().database_file);
}

fs::path restore_home(const fs::path& root) {
    return root / fv::path_from_utf8(names().restore_dir);
}

CreateOptions options_for(const fs::path& root, bool include_recordings = true) {
    CreateOptions options;
    options.data_root = root;
    options.include_recordings = include_recordings;
    options.app_version = "0.1.0";
    return options;
}

std::vector<std::string> member_names(const fs::path& archive) {
    ZipReader reader(archive);
    std::vector<std::string> out;
    for (const auto& info : reader.infolist()) {
        out.push_back(info.name);
    }
    return out;
}

std::string read_member(ZipReader& reader, std::string_view name) {
    std::string out;
    reader.read_entry(name, [&](std::span<const std::byte> block) {
        out.append(reinterpret_cast<const char*>(block.data()), block.size());
    });
    return out;
}

using Row = std::vector<fv::sql::Value>;

std::vector<Row> rows(const fs::path& db, std::string_view sql) {
    auto connection = fv::sql::Connection::open(db, fv::sql::OpenMode::ReadOnly);
    auto statement = connection.prepare(sql);
    std::vector<Row> out;
    while (statement.step()) {
        Row row;
        for (int i = 0; i < statement.column_count(); ++i) {
            row.push_back(statement.column(i));
        }
        out.push_back(std::move(row));
    }
    return out;
}

std::string text_of(const fv::sql::Value& value) {
    return std::get<std::string>(value);
}

bool is_null(const fv::sql::Value& value) {
    return std::holds_alternative<std::nullptr_t>(value);
}

Json read_json(const fs::path& path) {
    return fv::py::loads(read_bytes(path));
}

std::vector<fs::path> folders_named(const fs::path& folder, std::string_view prefix) {
    std::vector<fs::path> out;
    for (const auto& entry : fs::directory_iterator(folder)) {
        if (fv::path_to_utf8(entry.path().filename()).starts_with(prefix)) {
            out.push_back(entry.path());
        }
    }
    return out;
}

} // namespace

TEST_CASE("backup restores to a different root and keeps previous data", "[test_backup]") {
    // from tests/test_backup.py::test_backup_restores_to_a_different_root_and_keeps_previous_data
    // (the bundle side is in test_create_inspect.cpp)
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    const fs::path target = temp.path() / "target";
    make_sample_data(source);
    make_sample_data(target);
    write_bytes(target / "recordings" / "prior-only.wav", "previous");
    const fs::path archive = temp.path() / "portable.fvbackup";
    create_backup(archive, options_for(source));

    std::vector<std::string> phases;
    const fs::path stage = prepare_restore(
        archive, target, [&](std::string_view phase, std::uint64_t done, std::uint64_t total) {
            phases.push_back(std::string(phase) + " " + std::to_string(done) + "/" +
                             std::to_string(total));
        });
    CHECK(stage.parent_path() == restore_home(target));
    CHECK(fs::is_directory(stage / "data"));
    REQUIRE_FALSE(phases.empty());
    CHECK(phases.back() ==
          "stage " + std::to_string(phases.size()) + "/" + std::to_string(phases.size()));
    CHECK(pending_restore_exists(target));
    const Json pending = read_json(restore_home(target) / "pending.json");
    CHECK(pending.at("stage") == fv::path_to_utf8(stage.filename()));

    CHECK(apply_pending_restore(target));
    CHECK_FALSE(pending_restore_exists(target));
    CHECK_FALSE(fs::exists(stage));
    CHECK(read_bytes(target / "recordings" / "own.wav") == "RIFF" + std::string(200, 'a'));
    CHECK_FALSE(fs::exists(target / "recordings" / "prior-only.wav"));
    const auto previous = previous_data_dir(target);
    REQUIRE(previous.has_value());
    CHECK(previous->parent_path() == restore_home(target));
    CHECK_THAT(fv::path_to_utf8(previous->filename()),
               Catch::Matchers::Matches(R"(previous-\d{8}T\d{6}Z-[0-9a-f]{8})"));
    CHECK(read_bytes(*previous / "recordings" / "prior-only.wav") == "previous");
    CHECK(read_bytes(target / ".env") == "OPENAI_API_KEY=do-not-export");
    CHECK(read_json(restore_home(target) / "last_restore.json") ==
          Json{{"previous", fv::path_to_utf8(previous->filename())}});

    const auto meeting = rows(database_of(target), "SELECT spool_dir,host_token,guest_token,"
                                                   "asr_remote_json,app_pid FROM meeting_sessions");
    REQUIRE(meeting.size() == 1);
    CHECK(text_of(meeting[0][0]) == fv::path_to_utf8(target / "meetings" / "m1"));
    CHECK(text_of(meeting[0][1]) != "old-host-token");
    CHECK(text_of(meeting[0][2]) != "old-guest-token");
    CHECK(is_null(meeting[0][3]));
    CHECK(is_null(meeting[0][4]));
    CHECK(text_of(rows(database_of(target), "SELECT file_path FROM meeting_audio_chunks")[0][0]) ==
          fv::path_to_utf8(target / "meetings" / "m1" / "mic-000.wav"));
    CHECK(std::get<std::int64_t>(
              rows(database_of(target), "SELECT count(*) FROM record_sync")[0][0]) == 0);
    CHECK(text_of(rows(database_of(source), "SELECT host_token FROM meeting_sessions")[0][0]) ==
          "old-host-token");
}

TEST_CASE("captures round trip with backup and are optional assets", "[test_backup]") {
    // from tests/test_backup.py::test_captures_round_trip_with_backup_and_are_optional_assets
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    const fs::path target = temp.path() / "target";
    make_sample_data(source);
    const fs::path capture = source / "captures" / "2026-10-21_14-32-05";
    for (const char* name : {"walkthrough.pdf", "walkthrough.md", "walkthrough.json", "shot-1.png",
                             "draft.json", "audio.wav"}) {
        write_bytes(capture / name, name);
    }
    const fs::path archive = temp.path() / "with-captures.fvbackup";
    create_backup(archive, options_for(source));
    const auto members = member_names(archive);
    CHECK(std::ranges::find(members, "data/captures/2026-10-21_14-32-05/shot-1.png") !=
          members.end());
    prepare_restore(archive, target);
    CHECK(apply_pending_restore(target));
    for (const auto& entry : fs::directory_iterator(capture)) {
        CHECK(read_bytes(target / "captures" / capture.filename() / entry.path().filename()) ==
              read_bytes(entry.path()));
    }
    const fs::path text = temp.path() / "text-only.fvbackup";
    create_backup(text, options_for(source, false));
    CHECK(std::ranges::none_of(member_names(text), [](const std::string& name) {
        return name.starts_with("data/captures/");
    }));
}

TEST_CASE("failed apply rolls back entire owned set and can retry", "[test_backup]") {
    // from tests/test_backup.py::test_failed_apply_rolls_back_entire_owned_set_and_can_retry
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    const fs::path target = temp.path() / "target";
    make_sample_data(source);
    make_sample_data(target);
    write_bytes(target / "recordings" / "prior-only.wav", "prior");
    const std::string old_settings = read_bytes(settings_of(target));
    const fs::path archive = temp.path() / "good.fvbackup";
    create_backup(archive, options_for(source));
    prepare_restore(archive, target);

    {
        bool failed = false;
        RestoreFaults faults;
        faults.before_move = [&](const fs::path& from, const fs::path& to) {
            if (!failed && from.filename() == fv::path_from_utf8(names().database_file) &&
                from.parent_path().filename() == "data" && to == database_of(target)) {
                failed = true;
                throw std::runtime_error("injected install failure");
            }
        };
        const ScopedRestoreFaults installed(std::move(faults));
        CHECK_THROWS_WITH(apply_pending_restore(target), ContainsSubstring("injected"));
        CHECK(failed);
    }
    CHECK(read_bytes(settings_of(target)) == old_settings);
    CHECK(read_bytes(target / "recordings" / "prior-only.wav") == "prior");
    CHECK(pending_restore_exists(target));
    CHECK_FALSE(fs::exists(restore_home(target) / "journal.json"));
    CHECK(apply_pending_restore(target));
    CHECK_FALSE(fs::exists(target / "recordings" / "prior-only.wav"));
}

TEST_CASE("a crash between two moves rolls back at the next apply and the restore completes",
          "[backup]") {
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    const fs::path target = temp.path() / "target";
    make_sample_data(source);
    make_sample_data(target);
    write_bytes(target / "recordings" / "prior-only.wav", "prior");
    write_bytes(target / "scratchpad.txt", "old notes");
    const std::string old_settings = read_bytes(settings_of(target));
    const fs::path archive = temp.path() / "good.fvbackup";
    create_backup(archive, options_for(source));
    prepare_restore(archive, target);

    {
        // The live database has moved into previous-*; the process dies before the staged one
        // takes its place.
        RestoreFaults faults;
        faults.before_move = [&](const fs::path& from, const fs::path& to) {
            if (to == database_of(target) && from.parent_path().filename() == "data") {
                throw SimulatedCrash();
            }
        };
        const ScopedRestoreFaults installed(std::move(faults));
        CHECK_THROWS_AS(apply_pending_restore(target), SimulatedCrash);
    }
    const Json journal = read_json(restore_home(target) / "journal.json");
    CHECK(journal.at("phase") == "applying");
    CHECK_FALSE(fs::exists(database_of(target)));
    CHECK(pending_restore_exists(target));

    CHECK(apply_pending_restore(target));
    CHECK_FALSE(fs::exists(restore_home(target) / "journal.json"));
    CHECK_FALSE(pending_restore_exists(target));
    CHECK(read_bytes(target / "recordings" / "own.wav") == "RIFF" + std::string(200, 'a'));
    CHECK_FALSE(fs::exists(target / "recordings" / "prior-only.wav"));
    CHECK_FALSE(fs::exists(target / "scratchpad.txt"));
    const auto previous = previous_data_dir(target);
    REQUIRE(previous.has_value());
    CHECK(read_bytes(*previous / "recordings" / "prior-only.wav") == "prior");
    CHECK(read_bytes(*previous / "scratchpad.txt") == "old notes");
    CHECK(read_bytes(settings_of(*previous)) == old_settings);
    CHECK(text_of(rows(database_of(*previous), "SELECT host_token FROM meeting_sessions")[0][0]) ==
          "old-host-token");
    CHECK(text_of(rows(database_of(target), "SELECT host_token FROM meeting_sessions")[0][0]) !=
          "old-host-token");
}

TEST_CASE("rebased database can grow and import rescrubs settings", "[test_backup]") {
    // from tests/test_backup.py::test_rebased_database_can_grow_and_import_rescrubs_settings
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    const fs::path target = temp.path() / ("long-destination-" + std::string(65, 'x'));
    make_sample_data(source);
    {
        auto connection =
            fv::sql::Connection::open(database_of(source), fv::sql::OpenMode::ReadWrite);
        auto insert = connection.prepare("INSERT INTO meeting_audio_chunks VALUES (?,?)");
        const std::string chunk = fv::path_to_utf8(source / "meetings" / "m1" / "mic-000.wav");
        auto transaction = connection.begin();
        for (std::int64_t i = 2; i < 800; ++i) {
            insert.reset();
            insert.bind(1, i).bind(2, chunk);
            insert.step();
        }
        transaction.commit();
    }
    const fs::path archive = temp.path() / "portable.fvbackup";
    create_backup(archive, options_for(source));

    const fs::path crafted = temp.path() / "crafted.fvbackup";
    Json manifest;
    {
        ZipReader original(archive);
        manifest = fv::py::loads(read_member(original, "manifest.json"));
        Json settings = fv::py::loads(read_member(original, names().settings_member));
        settings["mcp_enabled"] = true;
        settings["remote_host_enabled"] = true;
        settings["remote_host_devices"] = Json::array({Json{{"token_hash", "stale"}}});
        settings["hidden_secret"] = "do-not-import";
        settings["text_llm_profiles"] = Json::array(
            {Json{{"base_url", "https://person:password@example.com/v1?token=secret#fragment"}}});
        const std::string settings_bytes = fv::py::dumps(settings);
        for (auto& item : manifest["files"]) {
            if (item["path"] == names().settings_member) {
                item["size"] = settings_bytes.size();
                item["sha256"] = fv::sha256_hex(settings_bytes);
            }
        }
        ZipWriter output = ZipWriter::create(crafted);
        for (const auto& info : original.infolist()) {
            std::string data;
            if (info.name == "manifest.json") {
                data = fv::py::dumps(manifest);
            } else if (info.name == names().settings_member) {
                data = settings_bytes;
            } else {
                data = read_member(original, info.name);
            }
            output.add_bytes(info.name, std::as_bytes(std::span(data.data(), data.size())),
                             info.method, info.external_attr);
        }
        output.close();
    }
    prepare_restore(crafted, target);
    const Json pending = read_json(restore_home(target) / "pending.json");
    const fs::path staged_db = restore_home(target) /
                               fv::path_from_utf8(pending.at("stage").get<std::string>()) / "data" /
                               fv::path_from_utf8(names().database_file);
    std::uint64_t archived_db_size = 0;
    for (const auto& item : manifest["files"]) {
        if (item["path"] == names().database_member) {
            archived_db_size = item["size"].get<std::uint64_t>();
        }
    }
    CHECK(fs::file_size(staged_db) > archived_db_size);
    CHECK(apply_pending_restore(target));
    const Json restored = read_json(settings_of(target));
    CHECK(restored.at("mcp_enabled") == false);
    CHECK(restored.at("remote_host_enabled") == false);
    CHECK_FALSE(restored.contains("remote_host_devices"));
    CHECK_FALSE(restored.contains("hidden_secret"));
    CHECK(restored.at("text_llm_profiles")[0].at("base_url") == "https://example.com/v1");
}

TEST_CASE("restore moves stale sqlite sidecars and recovers committed journal", "[test_backup]") {
    // from
    // tests/test_backup.py::test_restore_moves_stale_sqlite_sidecars_and_recovers_committed_journal
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    const fs::path target = temp.path() / "target";
    make_sample_data(source);
    make_sample_data(target);
    const fs::path archive = temp.path() / "portable.fvbackup";
    create_backup(archive, options_for(source));
    prepare_restore(archive, target);

    // Real SQLite WAL and SHM bytes, captured before close and put back, model a crash after
    // staging: the next startup must take their replay path away from the imported database.
    const fs::path db = database_of(target);
    const fs::path wal = fs::path(db).concat("-wal");
    const fs::path shm = fs::path(db).concat("-shm");
    const fs::path rollback_journal = fs::path(db).concat("-journal");
    std::string wal_bytes;
    std::string shm_bytes;
    {
        auto connection = fv::sql::Connection::open(db, fv::sql::OpenMode::ReadWrite);
        connection.scalar("PRAGMA journal_mode=WAL");
        connection.exec("PRAGMA wal_autocheckpoint=0");
        connection.exec("UPDATE meeting_sessions SET host_token='old-wal-token'");
        wal_bytes = read_bytes(wal);
        shm_bytes = read_bytes(shm);
    }
    REQUIRE(wal_bytes.size() > 32);
    REQUIRE(shm_bytes.size() > 32);
    write_bytes(wal, wal_bytes);
    write_bytes(shm, shm_bytes);
    write_bytes(rollback_journal, "old-rollback-journal");
    const fs::path journal_path = restore_home(target) / "journal.json";

    {
        bool failed = false;
        RestoreFaults faults;
        faults.before_remove = [&](const fs::path& path) {
            if (path == journal_path && !failed) {
                failed = true;
                throw std::runtime_error("injected committed-journal interruption");
            }
        };
        const ScopedRestoreFaults installed(std::move(faults));
        CHECK_THROWS_WITH(apply_pending_restore(target), ContainsSubstring("committed-journal"));
    }
    CHECK_FALSE(fs::exists(restore_home(target) / "pending.json"));
    CHECK(fs::exists(journal_path));
    CHECK(pending_restore_exists(target));
    CHECK(apply_pending_restore(target));
    const auto previous = previous_data_dir(target);
    REQUIRE(previous.has_value());
    const std::pair<const char*, std::string> sidecars[] = {
        {"-wal", wal_bytes}, {"-shm", shm_bytes}, {"-journal", "old-rollback-journal"}};
    for (const auto& [suffix, expected] : sidecars) {
        CHECK_FALSE(fs::exists(fs::path(db).concat(suffix)));
        CHECK(read_bytes(fs::path(database_of(*previous)).concat(suffix)) == expected);
    }
    CHECK(text_of(rows(db, "SELECT host_token FROM meeting_sessions")[0][0]) != "old-wal-token");
}

TEST_CASE("real database manager wal roundtrip", "[test_backup]") {
    // from tests/test_backup.py::test_real_database_manager_wal_roundtrip, parametrized over
    // include_recordings; the history row and the meeting are written through Database's own
    // schema (OpenWhisper uses DatabaseManager and SqlMeetingRepository)
    const bool include_recordings = GENERATE(true, false);
    CAPTURE(include_recordings);
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    const fs::path target = temp.path() / "target";
    fs::create_directories(target);
    write_bytes(source / "recordings" / "dictation.wav", "RIFFhistory");
    const fs::path spool = source / "meetings" / "meeting-real";
    const fs::path chunk_path = spool / "mic-000.wav";
    write_bytes(chunk_path, "RIFFmeeting");
    const fs::path archive =
        temp.path() / (include_recordings ? "real-full.fvbackup" : "real-text.fvbackup");
    {
        fv::Database source_db = fv::Database::open_or_create(database_of(source));
        source_db.write([&](fv::sql::Connection& connection) {
            connection
                .prepare("INSERT INTO transcription_history (id, text, timestamp, model, "
                         "audio_file) VALUES ('history-real', 'dictation text', "
                         "'2026-10-04T00:00:00Z', 'local', 'dictation.wav')")
                .step();
            auto meeting = connection.prepare(
                "INSERT INTO meeting_sessions (id, title, status, started_at, ended_at, "
                "paused_total_s, host_token, guest_token, cloud_enabled, spool_dir, state_seq) "
                "VALUES ('meeting-real', 'Real meeting', 'ended', '2026-10-04T00:00:00Z', "
                "'2026-10-04T00:01:00Z', 0.0, 'old-host', 'old-guest', 0, ?, 0)");
            meeting.bind(1, fv::path_to_utf8(spool)).step();
            auto chunk = connection.prepare(
                "INSERT INTO meeting_audio_chunks (meeting_id, channel, seq, file_path, start_s, "
                "duration_s, sample_rate, asr_status, asr_attempts) VALUES ('meeting-real', "
                "'mic', 0, ?, 0.0, 1.0, 16000, 'pending', 0)");
            chunk.bind(1, fv::path_to_utf8(chunk_path)).step();
            auto segment = connection.prepare(
                "INSERT INTO meeting_segments (id, meeting_id, chunk_id, channel, start_s, end_s, "
                "text, speaker_source, speaker_pinned, created_at) VALUES ('segment-real', "
                "'meeting-real', ?, 'mic', 0.0, 1.0, 'hello', 'channel', 0, "
                "'2026-10-04T00:00:00Z')");
            segment.bind(1, connection.last_insert_rowid()).step();
        });
        CHECK(fs::exists(fs::path(database_of(source)).concat("-wal")));
        create_backup(archive, options_for(source, include_recordings));
    }
    prepare_restore(archive, target);
    CHECK(apply_pending_restore(target));

    fv::Database target_db = fv::Database::open_or_create(database_of(target));
    target_db.read([&](fv::sql::Connection& connection) {
        auto meeting =
            connection.prepare("SELECT title, spool_dir FROM meeting_sessions WHERE id = "
                               "'meeting-real'");
        REQUIRE(meeting.step());
        CHECK(text_of(meeting.column(0)) == "Real meeting");
        CHECK(text_of(meeting.column(1)) == fv::path_to_utf8(target / "meetings" / "meeting-real"));
        auto history = connection.prepare(
            "SELECT text, audio_file FROM transcription_history WHERE id='history-real'");
        REQUIRE(history.step());
        CHECK(text_of(history.column(0)) == "dictation text");
        if (include_recordings) {
            CHECK(text_of(history.column(1)) == "dictation.wav");
        } else {
            CHECK(is_null(history.column(1)));
        }
        std::vector<std::pair<std::int64_t, std::string>> chunks;
        auto chunk_rows = connection.prepare("SELECT id, file_path FROM meeting_audio_chunks");
        while (chunk_rows.step()) {
            chunks.emplace_back(std::get<std::int64_t>(chunk_rows.column(0)),
                                text_of(chunk_rows.column(1)));
        }
        auto segment = connection.prepare("SELECT chunk_id FROM meeting_segments");
        REQUIRE(segment.step());
        CHECK(chunks.size() == (include_recordings ? 1U : 0U));
        if (include_recordings) {
            REQUIRE(chunks.size() == 1);
            CHECK(std::get<std::int64_t>(segment.column(0)) == chunks[0].first);
            CHECK(chunks[0].second ==
                  fv::path_to_utf8(target / "meetings" / "meeting-real" / "mic-000.wav"));
        } else {
            CHECK(is_null(segment.column(0)));
        }
        CHECK_FALSE(segment.step());
    });
}

TEST_CASE("scratchpad is backed up and restored", "[test_personalize_foundation_backup]") {
    // from tests/test_personalize_foundation_backup.py::test_scratchpad_is_backed_up_and_restored
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    const fs::path target = temp.path() / "target";
    fs::create_directories(target);
    write_bytes(source / "scratchpad.txt", "Groceries:\n- oat milk\n");
    write_bytes(settings_of(source), "{}");
    const fs::path archive = temp.path() / "notes.fvbackup";

    CHECK(names().is_owned("scratchpad.txt"));
    create_backup(archive, options_for(source));
    const auto members = member_names(archive);
    CHECK(std::ranges::find(members, "data/scratchpad.txt") != members.end());

    prepare_restore(archive, target);
    CHECK(apply_pending_restore(target));
    CHECK(read_bytes(target / "scratchpad.txt") == "Groceries:\n- oat milk\n");
}

TEST_CASE("snippet links with queries survive a backup and restore",
          "[test_personalize_foundation_backup]") {
    // from
    // tests/test_personalize_foundation_backup.py::test_snippet_links_with_queries_survive_a_backup_and_restore
    // (load_snippets is M4's: the restored list is compared with Snippet.to_dict() itself)
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    const fs::path target = temp.path() / "target";
    fs::create_directories(target);
    const Json snippet = {
        {"id", "cal"},
        {"trigger", "my calendar link"},
        {"text", "Book here: [a time](https://cal.example.com/dana?ref=email&src=ow#slots)"},
        {"formatted", true}};
    Json settings = Json::object();
    settings["dictation_snippets"] = Json::array({snippet});
    settings["transcript_cleanup_base_url"] = "https://user:pw@api.example.com/v1?key=abc";
    write_bytes(settings_of(source), fv::py::dumps(settings));
    const fs::path archive = temp.path() / "snippets.fvbackup";

    create_backup(archive, options_for(source));
    prepare_restore(archive, target);
    CHECK(apply_pending_restore(target));

    const Json restored = read_json(settings_of(target));
    CHECK(restored.at("dictation_snippets") == Json::array({snippet}));
    CHECK(restored.at("transcript_cleanup_base_url") == "https://api.example.com/v1");
}

TEST_CASE("an OpenWhisper backup restores into FioriVoce's file names", "[backup]") {
    // tests/fixtures/backup/openwhisper-sample.owbackup was made by OpenWhisper's create_backup.
    fv::testkit::TempDataRoot temp;
    const fs::path archive = fv::testkit::source_root() / "tests" / "fixtures" / "backup" /
                             "openwhisper-sample.owbackup";
    const fs::path target = temp.path() / "target";
    const fs::path stage = prepare_restore(archive, target);

    // The stage is FioriVoce's: its manifest names FioriVoce's format and files.
    const Json manifest = read_json(stage / "manifest.json");
    CHECK(manifest.at("format") == names().format);
    std::vector<std::string> listed;
    for (const auto& item : manifest.at("files")) {
        listed.push_back(item.at("path").get<std::string>());
    }
    CHECK(std::ranges::find(listed, names().settings_member) != listed.end());
    CHECK(std::ranges::find(listed, names().database_member) != listed.end());
    CHECK(std::ranges::none_of(listed, [](const std::string& path) {
        return path.find("openwhisper") != std::string::npos;
    }));

    CHECK(apply_pending_restore(target));
    CHECK(fs::is_regular_file(settings_of(target)));
    CHECK(fs::is_regular_file(database_of(target)));
    for (const auto& entry : fs::directory_iterator(target)) {
        CHECK(fv::path_to_utf8(entry.path().filename()).find("openwhisper") == std::string::npos);
    }
    const Json settings = read_json(settings_of(target));
    CHECK(settings.at("ui_theme") == "dark");
    CHECK(settings.at("mcp_enabled") == false);
    CHECK(settings.at("transcript_cleanup_base_url") == "https://api.example.com/v1");
    // The generator ran on Windows, where write_text wrote CRLF line ends.
    CHECK(read_bytes(target / "scratchpad.txt") == "Groceries:\r\n- oat milk\r\n");
    CHECK(fs::is_regular_file(target / "captures" / "2026-10-21_14-32-05" / "shot-1.png"));
    const auto meeting = rows(database_of(target), "SELECT spool_dir FROM meeting_sessions");
    REQUIRE(meeting.size() == 1);
    CHECK(text_of(meeting[0][0]) == fv::path_to_utf8(target / "meetings" / "meeting-ended"));
    const auto history =
        rows(database_of(target), "SELECT id FROM transcription_history ORDER BY id");
    CHECK(history.size() == 3);
}

TEST_CASE("a restore leaves no stage behind when the bundle is refused", "[backup]") {
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    const fs::path target = temp.path() / "target";
    make_sample_data(source);
    const fs::path archive = temp.path() / "good.fvbackup";
    create_backup(archive, options_for(source));

    CHECK_THROWS_MATCHES(prepare_restore(temp.path() / "missing.fvbackup", target), BackupError,
                         Message("Backup file does not exist"));
    prepare_restore(archive, target);
    CHECK_THROWS_MATCHES(prepare_restore(archive, target), BackupError,
                         Message("A restore is already waiting for restart"));
    CHECK(folders_named(restore_home(target), "stage-").size() == 1);

    // A journal left by an unfinished restore must be recovered before another stage.
    fs::rename(restore_home(target) / "pending.json", temp.path() / "pending.json");
    write_bytes(restore_home(target) / "journal.json", "{}");
    CHECK_THROWS_MATCHES(prepare_restore(archive, target), BackupError,
                         Message("An earlier restore requires recovery before staging another"));
    CHECK(folders_named(restore_home(target), "stage-").size() == 1);
}

TEST_CASE("a live meeting in the current data refuses a restore", "[backup]") {
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    const fs::path target = temp.path() / "target";
    make_sample_data(source);
    make_sample_data(target, true);
    const fs::path archive = temp.path() / "good.fvbackup";
    create_backup(archive, options_for(source));
    CHECK_THROWS_MATCHES(prepare_restore(archive, target), BackupError,
                         Message("End the live meeting before preparing a restore"));
    CHECK(folders_named(restore_home(target), "stage-").empty());
    CHECK_FALSE(pending_restore_exists(target));
}

TEST_CASE("nothing pending applies nothing, and a changed stage is refused before any move",
          "[backup]") {
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    const fs::path target = temp.path() / "target";
    make_sample_data(source);
    make_sample_data(target);
    CHECK_FALSE(apply_pending_restore(target));
    CHECK_FALSE(previous_data_dir(target).has_value());

    const fs::path archive = temp.path() / "good.fvbackup";
    create_backup(archive, options_for(source));
    const fs::path stage = prepare_restore(archive, target);
    const std::string settings_before = read_bytes(settings_of(target));

    SECTION("a staged file changed") {
        write_bytes(stage / "data" / "recordings" / "own.wav", "RIFFchanged");
        CHECK_THROWS_MATCHES(apply_pending_restore(target), BackupError,
                             Message("Staged restore file is missing or changed"));
    }
    SECTION("a file nobody listed") {
        write_bytes(stage / "data" / "recordings" / "extra.wav", "RIFF");
        CHECK_THROWS_MATCHES(apply_pending_restore(target), BackupError,
                             Message("Staged restore contains unlisted data"));
    }
    SECTION("the staged settings rewritten") {
        write_bytes(stage / "data" / fv::path_from_utf8(names().settings_file), "{\"x\": 1}  ");
        CHECK_THROWS_MATCHES(apply_pending_restore(target), BackupError,
                             Message("Staged restore file is missing or changed"));
    }
    SECTION("pending names no stage") {
        write_bytes(restore_home(target) / "pending.json", R"({"stage":"stage-../x"})");
        CHECK_THROWS_MATCHES(apply_pending_restore(target), BackupError,
                             Message("Pending restore metadata is unsafe"));
    }
    CHECK(read_bytes(settings_of(target)) == settings_before);
    CHECK(folders_named(restore_home(target), "previous-").empty());
    CHECK(pending_restore_exists(target));
}

TEST_CASE("an unsafe journal is refused and an unknown phase is not recovered", "[backup]") {
    fv::testkit::TempDataRoot temp;
    const fs::path root = temp.path() / "root";
    const fs::path home = restore_home(root);
    const std::string stage = "stage-" + std::string(32, 'a');
    fs::create_directories(home / stage);
    fs::create_directories(home / "previous-x");

    SECTION("unsafe names") {
        write_bytes(
            home / "journal.json",
            R"({"phase":"applying","stage":"../x","previous":"previous-x","stage_names":[]})");
        CHECK_THROWS_MATCHES(apply_pending_restore(root), BackupError,
                             Message("Restore journal is unsafe"));
    }
    SECTION("names outside the restore set") {
        write_bytes(home / "journal.json",
                    R"({"phase":"applying","stage":")" + stage +
                        R"(","previous":"previous-x","stage_names":[".env"]})");
        CHECK_THROWS_MATCHES(apply_pending_restore(root), BackupError,
                             Message("Restore journal has unsafe path metadata"));
    }
    SECTION("duplicate names") {
        write_bytes(
            home / "journal.json",
            R"({"phase":"applying","stage":")" + stage +
                R"(","previous":"previous-x","stage_names":["scratchpad.txt","scratchpad.txt"]})");
        CHECK_THROWS_MATCHES(apply_pending_restore(root), BackupError,
                             Message("Restore journal has unsafe path metadata"));
    }
    SECTION("an unknown phase") {
        write_bytes(home / "journal.json", R"({"phase":"other","stage":")" + stage +
                                               R"(","previous":"previous-x","stage_names":[]})");
        CHECK_THROWS_MATCHES(apply_pending_restore(root), BackupError,
                             Message("Restore journal cannot be recovered automatically"));
    }
    CHECK(fs::exists(home / "journal.json"));
}
