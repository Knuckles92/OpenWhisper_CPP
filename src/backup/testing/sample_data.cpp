#include "backup/testing/sample_data.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "base/json.h"
#include "base/utf8.h"
#include "db/schema.h"
#include "pyport/json.h"
#include "sql/db.h"

namespace fv::backup::testing {

void write_bytes(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw std::runtime_error("cannot write " + path_to_utf8(path));
    }
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void make_sample_data(const std::filesystem::path& root, bool live, const AppIdentity& names) {
    std::filesystem::create_directories(root / "recordings");
    write_bytes(root / "recordings" / "own.wav", "RIFF" + std::string(200, 'a'));
    write_bytes(root / "recordings" / "devices" / "peer" / "remote.wav", "RIFFremote");
    const std::filesystem::path chunk = root / "meetings" / "m1" / "mic-000.wav";
    write_bytes(chunk, "RIFF" + std::string(256, 'b'));
    write_bytes(root / "remote_records" / "cache" / "throwaway.wav", "cached");
    write_bytes(root / "remote_records" / "retained" / "note.json", "{}");
    write_bytes(root / "recorded_audio.wav", "RIFFrecovery");
    write_bytes(root / ".env", "OPENAI_API_KEY=do-not-export");
    write_bytes(root / "remote_engine_host_key.pem", "private-key");

    Json settings = Json::object();
    settings["ui_theme"] = "dark";
    settings["remote_engine_client"] = Json{{"host", "elsewhere"}, {"fingerprint", "old"}};
    settings["remote_host_enabled"] = true;
    settings["remote_host_devices"] = Json::array({Json{{"token_hash", "old-token-digest"}}});
    settings["remote_records_location"] = "host";
    settings["mcp_enabled"] = true;
    settings["meeting_context_folder_path"] = path_to_utf8(root / "external");
    settings["legacy_secret"] = "do-not-export";
    write_bytes(root / path_from_utf8(names.settings_file), py::dumps(settings));

    sql::Connection conn =
        sql::Connection::open(root / path_from_utf8(names.database_file), sql::OpenMode::Create);
    conn.exec(R"(
        CREATE TABLE schema_version (version INTEGER PRIMARY KEY);
        CREATE TABLE transcription_history (
            id TEXT PRIMARY KEY, audio_file TEXT);
        CREATE TABLE meeting_sessions (
            id TEXT PRIMARY KEY, status TEXT, spool_dir TEXT,
            host_token TEXT, guest_token TEXT, asr_remote_json TEXT,
            app_pid INTEGER, app_heartbeat_at TEXT);
        CREATE TABLE meeting_audio_chunks (
            id INTEGER PRIMARY KEY, file_path TEXT);
        CREATE TABLE meeting_segments (
            id TEXT PRIMARY KEY, chunk_id INTEGER);
        CREATE TABLE record_sync (kind TEXT, record_id TEXT);
    )");
    sql::Statement version = conn.prepare("INSERT INTO schema_version VALUES (?)");
    version.bind(1, std::int64_t{schema::schema_version});
    version.step();
    conn.exec("INSERT INTO transcription_history VALUES ('h1','own.wav')");
    conn.exec("INSERT INTO transcription_history VALUES ('h2','devices/peer/remote.wav')");
    sql::Statement meeting = conn.prepare("INSERT INTO meeting_sessions VALUES (?,?,?,?,?,?,?,?)");
    meeting.bind(1, std::string("m1"))
        .bind(2, std::string(live ? "active" : "ended"))
        .bind(3, path_to_utf8(root / "meetings" / "m1"))
        .bind(4, std::string("old-host-token"))
        .bind(5, std::string("old-guest-token"))
        .bind(6, std::string(R"({"host":"old"})"))
        .bind(7, std::int64_t{123})
        .bind(8, std::string("yesterday"));
    meeting.step();
    sql::Statement chunk_row = conn.prepare("INSERT INTO meeting_audio_chunks VALUES (?,?)");
    chunk_row.bind(1, std::int64_t{1}).bind(2, path_to_utf8(chunk));
    chunk_row.step();
    conn.exec("INSERT INTO meeting_segments VALUES ('s1',1)");
    conn.exec("INSERT INTO record_sync VALUES ('meeting','m1')");
}

} // namespace fv::backup::testing
