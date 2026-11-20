// Creating and inspecting bundles: the create-side safety tests of OpenWhisper's
// tests/test_backup.py, and the OpenWhisper_CPP bundle against OpenWhisper's own.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "archive/zip_reader.h"
#include "archive/zip_writer.h"
#include "backup/backup.h"
#include "backup/create.h"
#include "backup/inspect.h"
#include "backup/names.h"
#include "backup/sanitize.h"
#include "backup/testing/sample_data.h"
#include "base/cancel.h"
#include "base/hash.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "db/database.h"
#include "db/schema.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "sql/db.h"
#include "testkit/testkit.h"

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Matches;
using fv::Json;
using fv::archive::Method;
using fv::archive::ZipReader;
using fv::archive::ZipWriter;
using fv::backup::BackupCancelled;
using fv::backup::BackupError;
using fv::backup::BackupInfo;
using fv::backup::create_backup;
using fv::backup::CreateOptions;
using fv::backup::inspect_backup;
using fv::backup::testing::make_sample_data;
using fv::backup::testing::read_bytes;
using fv::backup::testing::write_bytes;

namespace {

const fv::backup::Names& names() {
    return fv::backup::names_for(fv::identity());
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

bool contains(const std::vector<std::string>& names, std::string_view name) {
    return std::ranges::find(names, name) != names.end();
}

std::string read_member(const fs::path& archive, std::string_view name) {
    ZipReader reader(archive);
    std::string out;
    reader.read_entry(name, [&](std::span<const std::byte> block) {
        out.append(reinterpret_cast<const char*>(block.data()), block.size());
    });
    return out;
}

std::span<const std::byte> bytes_of(const std::string& text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

// Copies `original` to `output` entry by entry, as zipfile.writestr(info, data) copies each
// ZipInfo: `change` may replace an entry's bytes or drop it (nullopt).
void rewrite(const fs::path& original, const fs::path& output,
             const std::function<std::optional<std::string>(const std::string& name,
                                                            std::string data)>& change) {
    ZipReader reader(original);
    ZipWriter writer = ZipWriter::create(output);
    for (const auto& info : reader.infolist()) {
        std::string data;
        reader.read_entry(info.name, [&](std::span<const std::byte> block) {
            data.append(reinterpret_cast<const char*>(block.data()), block.size());
        });
        const std::optional<std::string> kept = change(info.name, std::move(data));
        if (kept) {
            writer.add_bytes(info.name, bytes_of(*kept), info.method, info.external_attr);
        }
    }
    writer.close();
}

// The archive's database snapshot, copied out to `folder`.
fs::path extract_database(const fs::path& archive, const fs::path& folder) {
    const fs::path path = folder / "snapshot.db";
    write_bytes(path, read_member(archive, names().database_member));
    return path;
}

std::vector<std::vector<fv::sql::Value>> rows(const fs::path& db, std::string_view sql) {
    fv::sql::Connection connection = fv::sql::Connection::open(db, fv::sql::OpenMode::ReadOnly);
    fv::sql::Statement statement = connection.prepare(sql);
    std::vector<std::vector<fv::sql::Value>> out;
    while (statement.step()) {
        std::vector<fv::sql::Value> row;
        for (int i = 0; i < statement.column_count(); ++i) {
            row.push_back(statement.column(i));
        }
        out.push_back(std::move(row));
    }
    return out;
}

bool is_null(const fv::sql::Value& value) {
    return std::holds_alternative<std::nullptr_t>(value);
}

std::string text_of(const fv::sql::Value& value) {
    return std::get<std::string>(value);
}

bool no_build_folders(const fs::path& folder) {
    for (const auto& entry : fs::directory_iterator(folder)) {
        if (fv::path_to_utf8(entry.path().filename()).starts_with(".fvbackup-build-")) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("a bundle inspects to the BackupInfo its creation returned", "[test_backup]") {
    // The create side of
    // tests/test_backup.py::test_backup_restores_to_a_different_root_and_keeps_previous_data
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    make_sample_data(source);
    const fs::path archive = temp.path() / "portable.fvbackup";
    std::vector<std::string> phases;
    CreateOptions options = options_for(source);
    options.progress = [&](std::string_view phase, std::uint64_t done, std::uint64_t total) {
        phases.push_back(std::string(phase) + " " + std::to_string(done) + "/" +
                         std::to_string(total));
    };

    const BackupInfo info = create_backup(archive, options);
    CHECK(info == inspect_backup(archive));
    CHECK(info.path == archive);
    CHECK(info.include_recordings);
    CHECK(info.file_count >= 6);
    CHECK_FALSE(info.legacy_format);
    CHECK(info.app_version == "0.1.0");
    CHECK(info.schema_version == fv::schema::schema_version);
    CHECK_THAT(info.created_at, Matches(R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\+00:00)"));
    CHECK(info.archive_bytes == fs::file_size(archive));
    CHECK(std::ranges::any_of(info.warnings, [](const std::string& warning) {
        return warning.find("Host-kept-only") != std::string::npos;
    }));
    REQUIRE(phases.size() == info.file_count + 1);
    CHECK(phases.front() == "scan " + std::to_string(info.file_count - 2) + "/" +
                                std::to_string(info.file_count - 2));
    CHECK(phases.back() ==
          "archive " + std::to_string(info.file_count) + "/" + std::to_string(info.file_count));

    const auto members = member_names(archive);
    CHECK(contains(members, "data/recordings/own.wav"));
    CHECK(std::ranges::none_of(
        members, [](const std::string& name) { return name.starts_with("data/remote_records/"); }));
    for (const auto& name : members) {
        CHECK(name.find(".env") == std::string::npos);
        CHECK(name.find("remote_engine_host_key.pem") == std::string::npos);
    }
    CHECK(members[0] == names().settings_member);
    CHECK(members[1] == names().database_member);
    CHECK(members.back() == "manifest.json");
    const Json settings = fv::py::loads(read_member(archive, names().settings_member));
    CHECK(settings.at("ui_theme") == "dark");
    CHECK(settings.at("mcp_enabled") == false);
    CHECK(settings.at("remote_records_location") == "local");
    CHECK_FALSE(settings.contains("remote_engine_client"));
    CHECK_FALSE(settings.contains("remote_host_devices"));
    CHECK_FALSE(settings.contains("legacy_secret"));

    // The snapshot's meeting is portable and holds no live secret; the source keeps its own.
    const fs::path snapshot = extract_database(archive, temp.path());
    const auto meeting = rows(snapshot, "SELECT spool_dir, host_token, guest_token, "
                                        "asr_remote_json, app_pid FROM meeting_sessions");
    REQUIRE(meeting.size() == 1);
    CHECK(text_of(meeting[0][0]) == "__OW_BACKUP_DATA__/meetings/m1");
    CHECK(text_of(meeting[0][1]) != "old-host-token");
    CHECK(text_of(meeting[0][1]).size() == 43);
    CHECK(text_of(meeting[0][2]) != "old-guest-token");
    CHECK(is_null(meeting[0][3]));
    CHECK(is_null(meeting[0][4]));
    CHECK(text_of(rows(snapshot, "SELECT file_path FROM meeting_audio_chunks")[0][0]) ==
          "__OW_BACKUP_DATA__/meetings/m1/mic-000.wav");
    CHECK(rows(snapshot, "SELECT * FROM record_sync").empty());
    const auto source_tokens =
        rows(source / names().database_file, "SELECT host_token FROM meeting_sessions");
    CHECK(text_of(source_tokens[0][0]) == "old-host-token");
    CHECK(no_build_folders(temp.path()));
}

TEST_CASE("text only archive clears audio references", "[test_backup]") {
    // from tests/test_backup.py::test_text_only_archive_clears_audio_references (the restore
    // of the bundle is M3.4's half; here the snapshot it would restore is checked)
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    make_sample_data(source);
    // Captures are recordings too (the text-only half of
    // test_captures_round_trip_with_backup_and_are_optional_assets).
    write_bytes(source / "captures" / "2026-10-21_14-32-05" / "shot-1.png", "png");
    const fs::path archive = temp.path() / "text.fvbackup";
    const BackupInfo info = create_backup(archive, options_for(source, false));
    CHECK_FALSE(info.include_recordings);
    CHECK(std::ranges::none_of(member_names(archive), [](const std::string& name) {
        return name.starts_with("data/recordings/") || name.starts_with("data/meetings/") ||
               name.starts_with("data/captures/") || name == "data/recorded_audio.wav";
    }));
    const fs::path snapshot = extract_database(archive, temp.path());
    CHECK(is_null(
        rows(snapshot, "SELECT audio_file FROM transcription_history WHERE id='h1'")[0][0]));
    CHECK(std::get<std::int64_t>(
              rows(snapshot, "SELECT count(*) FROM meeting_audio_chunks")[0][0]) == 0);
    CHECK(is_null(rows(snapshot, "SELECT chunk_id FROM meeting_segments")[0][0]));
    CHECK(text_of(rows(snapshot, "SELECT spool_dir FROM meeting_sessions")[0][0]) ==
          "__OW_BACKUP_DATA__/meetings/m1");
}

TEST_CASE("live meeting is refused and existing output is unchanged", "[test_backup]") {
    // from tests/test_backup.py::test_live_meeting_is_refused_and_existing_output_is_unchanged
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    make_sample_data(source, true);
    const fs::path archive = temp.path() / "existing.fvbackup";
    write_bytes(archive, "old");
    CHECK_THROWS_MATCHES(create_backup(archive, options_for(source)), BackupError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("live meeting")));
    CHECK(read_bytes(archive) == "old");
    CHECK(no_build_folders(temp.path()));
}

TEST_CASE("cancel does not publish a partial archive", "[test_backup]") {
    // from tests/test_backup.py::test_cancel_does_not_publish_a_partial_archive
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    make_sample_data(source);
    const fs::path archive = temp.path() / "existing.fvbackup";
    write_bytes(archive, "old");
    fv::CancelSource cancel;
    cancel.cancel();
    CreateOptions options = options_for(source);
    options.cancel = cancel.token();
    CHECK_THROWS_AS(create_backup(archive, options), BackupCancelled);
    CHECK(read_bytes(archive) == "old");

    // Cancelled part way through the archive, too.
    fv::CancelSource later;
    options.cancel = later.token();
    options.progress = [&](std::string_view phase, std::uint64_t done, std::uint64_t) {
        if (phase == "archive" && done == 3) {
            later.cancel();
        }
    };
    CHECK_THROWS_AS(create_backup(archive, options), BackupCancelled);
    CHECK(read_bytes(archive) == "old");
    CHECK(no_build_folders(temp.path()));
}

TEST_CASE("destination inside not yet created recordings is rejected", "[test_backup]") {
    // from tests/test_backup.py::test_destination_inside_not_yet_created_recordings_is_rejected
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    fs::create_directories(source);
    const fs::path archive = source / "recordings" / "first.fvbackup";
    CHECK_THROWS_MATCHES(
        create_backup(archive, options_for(source)), BackupError,
        Catch::Matchers::MessageMatches(ContainsSubstring("outside recorded data")));
    CHECK_FALSE(fs::exists(archive.parent_path()));

    CHECK_THROWS_MATCHES(
        create_backup(source / names().restore_dir / "x.fvbackup", options_for(source)),
        BackupError, Catch::Matchers::Message("Choose a backup destination outside recorded data"));
    CHECK_THROWS_MATCHES(create_backup(temp.path() / "x.owbackup", options_for(source)),
                         BackupError,
                         Catch::Matchers::Message("Backup file must end in .fvbackup"));
    CHECK_THROWS_MATCHES(create_backup(temp.path() / ".fvbackup", options_for(source)), BackupError,
                         Catch::Matchers::Message("Backup file must end in .fvbackup"));
}

TEST_CASE("corrupt and traversal archives are rejected", "[test_backup]") {
    // from tests/test_backup.py::test_corrupt_and_traversal_archives_are_rejected
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    make_sample_data(source);
    const fs::path archive = temp.path() / "good.fvbackup";
    create_backup(archive, options_for(source));
    const fs::path corrupt = temp.path() / "corrupt.fvbackup";
    rewrite(archive, corrupt, [](const std::string& name, std::string data) {
        if (name == "data/recordings/own.wav") {
            data += "changed";
        }
        return std::optional<std::string>(std::move(data));
    });
    CHECK_THROWS_MATCHES(inspect_backup(corrupt), BackupError,
                         Catch::Matchers::MessageMatches(Matches(".*(metadata|checksum).*")));

    const fs::path traversal = temp.path() / "traversal.fvbackup";
    {
        ZipWriter output = ZipWriter::create(traversal);
        output.add_bytes("data/../outside", bytes_of("evil"), Method::Deflated, 0);
        output.add_bytes("manifest.json", bytes_of("{}"), Method::Deflated, 0);
        output.close();
    }
    CHECK_THROWS_MATCHES(inspect_backup(traversal), BackupError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("Unsafe")));

    const fs::path link_archive = temp.path() / "symlink.fvbackup";
    {
        ZipWriter output = ZipWriter::create(link_archive);
        output.add_bytes("data/recordings/link.wav", bytes_of("../../somewhere"), Method::Deflated,
                         (0120000u | 0777u) << 16);
        output.add_bytes("manifest.json", bytes_of("{}"), Method::Deflated, 0);
        output.close();
    }
    CHECK_THROWS_MATCHES(inspect_backup(link_archive), BackupError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("symbolic link")));

    const fs::path not_a_zip = temp.path() / "plain.fvbackup";
    write_bytes(not_a_zip, "not a zip at all");
    CHECK_THROWS_MATCHES(inspect_backup(not_a_zip), BackupError,
                         Catch::Matchers::Message("Backup ZIP is corrupt"));
    CHECK_THROWS_MATCHES(inspect_backup(temp.path() / "missing.fvbackup"), BackupError,
                         Catch::Matchers::Message("Backup file does not exist"));
}

TEST_CASE("checksums cannot hide missing database audio", "[test_backup]") {
    // from tests/test_backup.py::test_checksums_cannot_hide_missing_database_audio
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    make_sample_data(source);
    const fs::path archive = temp.path() / "good.fvbackup";
    create_backup(archive, options_for(source));
    const fs::path crafted = temp.path() / "missing-audio.fvbackup";
    const std::string missing = "data/meetings/m1/mic-000.wav";
    Json manifest = fv::py::loads(read_member(archive, "manifest.json"));
    Json files = Json::array();
    for (const auto& item : manifest.at("files")) {
        if (item.at("path") != missing) {
            files.push_back(item);
        }
    }
    manifest["files"] = files;
    rewrite(archive, crafted, [&](const std::string& name, std::string data) {
        if (name == missing) {
            return std::optional<std::string>();
        }
        return std::optional<std::string>(name == "manifest.json" ? fv::py::dumps(manifest)
                                                                  : std::move(data));
    });
    CHECK_THROWS_MATCHES(
        inspect_backup(crafted), BackupError,
        Catch::Matchers::MessageMatches(ContainsSubstring("missing meeting audio")));
}

TEST_CASE("legacy history is imported into snapshot without raw json", "[test_backup]") {
    // from tests/test_backup.py::test_legacy_history_is_imported_into_snapshot_without_raw_json
    // (the restore is M3.4's half; here the snapshot it would restore is read)
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "legacy";
    write_bytes(source / "recordings" / "old.wav", "RIFFold");
    write_bytes(source / "transcription_history.json",
                R"({"entries": [{"id": "legacy-id", "text": "from JSON", "timestamp": "now", )"
                R"("model": "local", "audio_file": "old.wav"}]})");
    const fs::path archive = temp.path() / "legacy.fvbackup";
    create_backup(archive, options_for(source));
    CHECK_FALSE(contains(member_names(archive), "data/transcription_history.json"));
    CHECK(contains(member_names(archive), "data/recordings/old.wav"));
    const auto history = rows(extract_database(archive, temp.path()),
                              "SELECT id, text, audio_file FROM transcription_history");
    REQUIRE(history.size() == 1);
    CHECK(text_of(history[0][0]) == "legacy-id");
    CHECK(text_of(history[0][1]) == "from JSON");
    CHECK(text_of(history[0][2]) == "old.wav");
}

TEST_CASE("a bundle of a v18 data root names nothing after OpenWhisper", "[backup]") {
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    fs::create_directories(source);
    write_bytes(source / "recordings" / "a.wav", "RIFFa");
    write_bytes(source / "meetings" / "m-ended" / "mic-000.wav", "RIFFm");
    write_bytes(source / "captures" / "2026-10-21_14-32-05" / "shot-1.png", "png");
    write_bytes(source / "captures" / "2026-10-21_14-32-05" / "walkthrough.json", "{}");
    write_bytes(source / "scratchpad.txt", "Groceries:\n- oat milk\n");
    write_bytes(source / names().settings_file,
                R"({"ui_theme": "light", "language": "fran\u00e7ais", "mcp_enabled": true})");
    {
        fv::Database database = fv::Database::open_or_create(source / names().database_file);
        database.write([&](fv::sql::Connection& connection) {
            connection.exec("INSERT INTO transcription_history (id, text, timestamp, model, "
                            "audio_file) VALUES ('h1', 'hello', '2026-10-04T00:00:00Z', "
                            "'parakeet', 'a.wav')");
            fv::sql::Statement meeting = connection.prepare(
                "INSERT INTO meeting_sessions (id, title, status, started_at, paused_total_s, "
                "host_token, guest_token, cloud_enabled, spool_dir, state_seq, app_pid) VALUES "
                "('m-ended', 'Ended', 'ended', '2026-10-04T00:00:00Z', 0.0, 'old-host', "
                "'old-guest', 0, ?, 0, 77)");
            meeting.bind(1, fv::path_to_utf8(source / "meetings" / "m-ended"));
            meeting.step();
            fv::sql::Statement chunk = connection.prepare(
                "INSERT INTO meeting_audio_chunks (meeting_id, channel, seq, file_path, start_s, "
                "duration_s, sample_rate, asr_status, asr_attempts) VALUES ('m-ended', 'mic', 0, "
                "?, 0.0, 1.0, 16000, 'done', 0)");
            chunk.bind(1, fv::path_to_utf8(source / "meetings" / "m-ended" / "mic-000.wav"));
            chunk.step();
        });
        database.close();
    }
    const fs::path archive = temp.path() / "v18.fvbackup";
    CreateOptions options = options_for(source);
    options.app_version = "0.1.0-\xc3\xa9";
    const BackupInfo info = create_backup(archive, options);
    CHECK(info == inspect_backup(archive));
    CHECK(info.warnings.empty());

    const auto members = member_names(archive);
    for (const auto& name : members) {
        CHECK(fv::py::lower(name).find("openwhisper") == std::string::npos);
    }
    CHECK(contains(members, "data/captures/2026-10-21_14-32-05/shot-1.png"));
    CHECK(contains(members, "data/scratchpad.txt"));
    CHECK(contains(members, "data/meetings/m-ended/mic-000.wav"));

    // manifest.json: compact, non-ASCII as itself.
    const std::string manifest_text = read_member(archive, "manifest.json");
    const Json manifest = fv::py::loads(manifest_text);
    fv::py::DumpOptions compact;
    compact.ensure_ascii = false;
    compact.item_sep = ",";
    compact.key_sep = ":";
    CHECK(fv::py::dumps(manifest, compact) == manifest_text);
    CHECK(manifest_text.find("0.1.0-\xc3\xa9") != std::string::npos);
    CHECK(manifest.at("format") == "openwhisper-cpp-backup");
    CHECK(manifest.at("format_version") == 1);
    CHECK(manifest.at("files")[0].at("path") == names().settings_member);

    // The settings member: indent 2 and a newline, in the text mode of the platform.
    fv::py::DumpOptions indented;
    indented.indent = 2;
    indented.ensure_ascii = false;
    std::string expected = fv::py::dumps(fv::backup::sanitize_settings(fv::py::loads(
                                             read_bytes(source / names().settings_file))),
                                         indented) +
                           "\n";
#ifdef _WIN32
    std::string crlf;
    for (const char c : expected) {
        crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    }
    expected = crlf;
#endif
    CHECK(read_member(archive, names().settings_member) == expected);
    CHECK(expected.find("fran\xc3\xa7"
                        "ais") != std::string::npos);

    // The snapshot is in rollback-journal mode, its ended meeting portable.
    const fs::path snapshot = extract_database(archive, temp.path());
    CHECK(text_of(rows(snapshot, "PRAGMA journal_mode")[0][0]) == "delete");
    const auto meeting =
        rows(snapshot, "SELECT spool_dir, host_token, app_pid FROM meeting_sessions");
    CHECK(text_of(meeting[0][0]) == "__OW_BACKUP_DATA__/meetings/m-ended");
    CHECK(text_of(meeting[0][1]) != "old-host");
    CHECK(is_null(meeting[0][2]));
    CHECK(text_of(rows(snapshot, "SELECT audio_file FROM transcription_history")[0][0]) == "a.wav");

    // A live meeting in the same root is refused.
    {
        fv::sql::Connection connection =
            fv::sql::Connection::open(source / names().database_file, fv::sql::OpenMode::ReadWrite);
        connection.exec("UPDATE meeting_sessions SET status='paused'");
    }
    CHECK_THROWS_MATCHES(create_backup(temp.path() / "live.fvbackup", options), BackupError,
                         Catch::Matchers::Message("End the live meeting before creating a backup"));
}

TEST_CASE("OpenWhisper's own bundle inspects with the counts OpenWhisper reported", "[backup]") {
    // tests/fixtures/backup/make_fixtures.py made both files with OpenWhisper's create_backup.
    const fs::path fixtures = fv::testkit::source_root() / "tests" / "fixtures" / "backup";
    const fs::path archive = fixtures / "openwhisper-sample.owbackup";
    const Json expected = fv::py::loads(read_bytes(fixtures / "openwhisper-sample.json"));
    const BackupInfo info = inspect_backup(archive);
    CHECK(info.legacy_format);
    CHECK(info.path == fs::absolute(archive));
    CHECK(info.created_at == expected.at("created_at").get<std::string>());
    CHECK(info.app_version == expected.at("app_version").get<std::string>());
    CHECK(info.schema_version == expected.at("schema_version").get<int>());
    CHECK(info.include_recordings == expected.at("include_recordings").get<bool>());
    CHECK(info.file_count == expected.at("file_count").get<std::uint64_t>());
    CHECK(info.total_bytes == expected.at("total_bytes").get<std::uint64_t>());
    CHECK(info.archive_bytes == expected.at("archive_bytes").get<std::uint64_t>());
    std::vector<std::string> warnings;
    for (const auto& warning : expected.at("warnings")) {
        warnings.push_back(warning.get<std::string>());
    }
    CHECK(info.warnings == warnings);

    // Its members carry OpenWhisper's names, which only the legacy reading accepts.
    const auto members = member_names(archive);
    CHECK(contains(members, "data/openwhisper_settings.json"));
    CHECK(contains(members, "data/openwhisper.db"));
}

TEST_CASE("a bundle with another product's format is unsupported", "[backup]") {
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    make_sample_data(source);
    const fs::path archive = temp.path() / "good.fvbackup";
    create_backup(archive, options_for(source));
    const fs::path other = temp.path() / "other.fvbackup";
    rewrite(archive, other, [](const std::string& name, std::string data) {
        if (name == "manifest.json") {
            Json manifest = fv::py::loads(data);
            manifest["format"] = "someone-else-backup";
            data = fv::py::dumps(manifest);
        }
        return std::optional<std::string>(std::move(data));
    });
    CHECK_THROWS_MATCHES(inspect_backup(other), BackupError,
                         Catch::Matchers::Message("Unsupported backup format"));
    // An OpenWhisper format id over OpenWhisper_CPP's member names: read with OpenWhisper's names,
    // OpenWhisper_CPP's settings file is outside its owned data.
    const fs::path mixed = temp.path() / "mixed.fvbackup";
    rewrite(archive, mixed, [](const std::string& name, std::string data) {
        if (name == "manifest.json") {
            Json manifest = fv::py::loads(data);
            manifest["format"] = "openwhisper-backup";
            data = fv::py::dumps(manifest);
        }
        return std::optional<std::string>(std::move(data));
    });
    CHECK_THROWS_MATCHES(inspect_backup(mixed), BackupError,
                         Catch::Matchers::Message("Archive contains a file outside owned data"));
}
