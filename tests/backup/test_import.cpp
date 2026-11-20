// Importing an OpenWhisper data folder (backup/import.h). OpenWhisper_CPP only: there is no
// OpenWhisper test to port. The OpenWhisper folders come from tests/fixtures/import (made by
// OpenWhisper's own services.database and services.settings) and are copied into a temporary folder
// first; nothing here reads an installed OpenWhisper.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "backup/coordinator.h"
#include "backup/detail/fs.h"
#include "backup/detail/staged.h"
#include "backup/import.h"
#include "backup/names.h"
#include "backup/restore.h"
#include "backup/testing/fake_hooks.h"
#include "backup/testing/sample_data.h"
#include "base/data_lease.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "lease_support.h"
#include "pyport/json.h"
#include "sql/db.h"
#include "testkit/testkit.h"

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::backup::ImportOptions;
using fv::backup::ImportResult;
using fv::backup::ImportSource;
using fv::backup::OpenWhisperRunning;
using fv::backup::stage_from_data_root;
using fv::backup::testing::read_bytes;
using fv::backup::testing::write_bytes;

namespace {

fs::path fixtures() {
    return fv::testkit::source_root() / "tests" / "fixtures" / "import";
}

std::string placeholder() {
    return fv::py::loads(read_bytes(fixtures() / "fixture.json")).at("placeholder");
}

// The fixture's paths start with a placeholder root: they become paths below `root`, as if
// OpenWhisper had run there.
std::string rebased(const std::string& text, const fs::path& root) {
    const std::string prefix = placeholder();
    if (!text.starts_with(prefix)) {
        return text;
    }
    fs::path path = root;
    std::string rest = text.substr(prefix.size());
    std::size_t at = 0;
    while (at < rest.size()) {
        const std::size_t next = rest.find('\\', at);
        const std::string part = rest.substr(at, next == std::string::npos ? next : next - at);
        if (!part.empty()) {
            path /= fv::path_from_utf8(part);
        }
        if (next == std::string::npos) {
            break;
        }
        at = next + 1;
    }
    return fv::path_to_utf8(path);
}

// A copy of the fixture folder `name` at `target`, its database paths made local.
fs::path install_fixture(std::string_view name, const fs::path& target) {
    fs::create_directories(target.parent_path());
    fs::copy(fixtures() / std::string(name), target, fs::copy_options::recursive);
    fv::sql::Connection connection =
        fv::sql::Connection::open(target / "openwhisper.db", fv::sql::OpenMode::ReadWrite);
    for (const auto& [table, column] :
         {std::pair<std::string, std::string>{"meeting_sessions", "spool_dir"},
          {"meeting_audio_chunks", "file_path"},
          {"transcription_history", "audio_file"}}) {
        std::vector<std::pair<fv::sql::Value, std::string>> rows;
        {
            auto select = connection.prepare("SELECT rowid, " + column + " FROM " + table +
                                             " WHERE " + column + " IS NOT NULL");
            while (select.step()) {
                rows.emplace_back(select.column(0), std::get<std::string>(select.column(1)));
            }
        }
        for (const auto& [rowid, text] : rows) {
            auto update =
                connection.prepare("UPDATE " + table + " SET " + column + "=? WHERE rowid=?");
            update.bind(1, rebased(text, target)).bind(2, rowid);
            update.step();
        }
    }
    connection.close();
    return target;
}

// Every entry below `root`: folders as "<dir>", files by their bytes.
std::map<std::string, std::string> tree(const fs::path& root) {
    std::map<std::string, std::string> entries;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const std::string name = fv::path_to_utf8(entry.path().lexically_relative(root));
        entries[name] = entry.is_directory() ? std::string("<dir>") : read_bytes(entry.path());
    }
    return entries;
}

bool same_file(const fs::path& a, const fs::path& b) {
    const auto x = fv::backup::detail::stat(a);
    const auto y = fv::backup::detail::stat(b);
    return x.device == y.device && x.inode == y.inode;
}

Json read_json(const fs::path& path) {
    return fv::py::loads(read_bytes(path));
}

// The single string a query answers.
std::string text_of(const fs::path& database, const std::string& sql) {
    fv::sql::Connection connection =
        fv::sql::Connection::open(database, fv::sql::OpenMode::ReadOnly);
    return std::get<std::string>(connection.scalar(sql));
}

std::int64_t count_of(const fs::path& database, const std::string& sql) {
    fv::sql::Connection connection =
        fv::sql::Connection::open(database, fv::sql::OpenMode::ReadOnly);
    return std::get<std::int64_t>(connection.scalar(sql));
}

// The data root as the app holds it: resolved (BackupCoordinator::data_dir), so that the stage
// and the start-up that applies it name the same folder even under an 8.3 temporary path.
fs::path resolved_temp(const fv::testkit::TempDataRoot& temp) {
    return fs::weakly_canonical(temp.path());
}

struct Folders {
    fv::testkit::TempDataRoot temp;
    fs::path source = install_fixture("openwhisper", resolved_temp(temp) / "OpenWhisper");
    fs::path root = resolved_temp(temp) / "OpenWhisper_CPP";
};

ImportOptions version(std::string app_version = "0.1.0") {
    ImportOptions options;
    options.app_version = std::move(app_version);
    return options;
}

ImportResult import_from(const Folders& folders, ImportOptions options = version()) {
    return stage_from_data_root(fv::legacy_identity(), folders.source, fv::identity(), folders.root,
                                std::move(options));
}

} // namespace

TEST_CASE("an OpenWhisper folder stages as a pending restore of the OpenWhisper_CPP folder",
          "[import]") {
    Folders folders;
    write_bytes(folders.root / "openwhisper_cpp_settings.json", "{\"ui_theme\": \"light\"}");
    const auto before = tree(folders.source);

    const ImportResult result = import_from(folders);

    const fs::path home = folders.root / ".openwhisper-cpp-restore";
    CHECK(result.stage.parent_path() == fs::weakly_canonical(home));
    CHECK(fv::backup::detail::file_sha256(result.stage / "manifest.json").size() == 64);
    CHECK(read_json(home / "pending.json").at("stage") ==
          fv::path_to_utf8(result.stage.filename()));
    CHECK(result.copied_keys.empty());
    // The stage passes the checks the start-up applies, before anything moves.
    CHECK_NOTHROW(fv::backup::detail::restore_stage_check(
        result.stage, fs::weakly_canonical(folders.root), fv::backup::names_for(fv::identity())));

    const fs::path data = result.stage / "data";
    std::set<std::string> staged;
    for (const auto& entry : fs::recursive_directory_iterator(data)) {
        if (entry.is_regular_file()) {
            staged.insert(fv::path_to_utf8(entry.path().lexically_relative(data)));
        }
    }
    const std::string sep(1, static_cast<char>(fs::path::preferred_separator));
    CHECK(staged == std::set<std::string>{
                        "openwhisper_cpp_settings.json",
                        "openwhisper-cpp.db",
                        "recorded_audio.wav",
                        "scratchpad.txt",
                        "recordings" + sep + "dictation.wav",
                        "recordings" + sep + "absolute.wav",
                        "meetings" + sep + "meeting-ended" + sep + "mic-000.wav",
                        "captures" + sep + "2026-10-21_14-32-05" + sep + "shot-1.png",
                    });

    SECTION("settings: the engine mapped, unknown keys kept, sanitized, the import recorded") {
        const Json settings = read_json(data / "openwhisper_cpp_settings.json");
        CHECK(settings.at("selected_model") == "parakeet");
        CHECK(settings.at("ui_theme") == "dark");
        CHECK(settings.at("fixture_future_key") == Json::parse(R"({"kept": true})"));
        CHECK(settings.at("mcp_enabled") == false);
        CHECK(settings.at("transcript_cleanup_base_url") == "https://api.example.com/v1");
        CHECK(settings.at("actions").at(0).at("enabled") == false);
        CHECK_FALSE(settings.at("text_llm_profiles").at(0).contains("api_key_env"));
        const Json record = settings.at("fv_import");
        CHECK(record.at("source") == fv::path_to_utf8(folders.source));
        CHECK(record.at("version") == "0.1.0");
        CHECK(record.at("at").get<std::string>().ends_with("+00:00"));
        std::vector<std::string> keys;
        for (const auto& [key, value] : record.items()) {
            keys.push_back(key);
        }
        CHECK(keys == std::vector<std::string>{"source", "at", "version"});
        CHECK(settings.items().begin().key() == "selected_model"); // OpenWhisper's order kept
        // Written as the app writes its settings: indent 2, CRLF on Windows.
        CHECK(read_bytes(data / "openwhisper_cpp_settings.json") ==
              fv::backup::detail::settings_text(settings));
    }

    SECTION("the database: a snapshot, its paths rebased onto the OpenWhisper_CPP folder") {
        const fs::path db = data / "openwhisper-cpp.db";
        CHECK(count_of(db, "SELECT COUNT(*) FROM transcription_history") == 3);
        CHECK(text_of(db, "SELECT audio_file FROM transcription_history WHERE "
                          "id='history-relative'") == "dictation.wav");
        CHECK(text_of(db, "SELECT audio_file FROM transcription_history WHERE "
                          "id='history-absolute'") == "absolute.wav");
        const fs::path root = fs::weakly_canonical(folders.root);
        CHECK(text_of(db, "SELECT spool_dir FROM meeting_sessions") ==
              fv::path_to_utf8(root / "meetings" / "meeting-ended"));
        CHECK(text_of(db, "SELECT file_path FROM meeting_audio_chunks") ==
              fv::path_to_utf8(root / "meetings" / "meeting-ended" / "mic-000.wav"));
        CHECK(text_of(db, "SELECT host_token FROM meeting_sessions") != "old-host");
        CHECK(text_of(db, "PRAGMA journal_mode") == "delete");
    }

    SECTION("recordings are hard-linked, files either app rewrites are copied") {
        CHECK(same_file(data / "recordings" / "dictation.wav",
                        folders.source / "recordings" / "dictation.wav"));
        CHECK(same_file(data / "meetings" / "meeting-ended" / "mic-000.wav",
                        folders.source / "meetings" / "meeting-ended" / "mic-000.wav"));
        CHECK(same_file(data / "captures" / "2026-10-21_14-32-05" / "shot-1.png",
                        folders.source / "captures" / "2026-10-21_14-32-05" / "shot-1.png"));
        CHECK_FALSE(same_file(data / "scratchpad.txt", folders.source / "scratchpad.txt"));
        CHECK_FALSE(same_file(data / "recorded_audio.wav", folders.source / "recorded_audio.wav"));
        CHECK(read_bytes(data / "scratchpad.txt") == read_bytes(folders.source / "scratchpad.txt"));
    }

    SECTION("the OpenWhisper folder is byte for byte as it was") {
        CHECK(tree(folders.source) == before);
    }

    SECTION("the next start applies it, keeping OpenWhisper_CPP's own data aside") {
        REQUIRE(fv::backup::apply_pending_restore(folders.root));
        CHECK(read_json(folders.root / "openwhisper_cpp_settings.json").at("selected_model") ==
              "parakeet");
        CHECK(count_of(folders.root / "openwhisper-cpp.db",
                       "SELECT COUNT(*) FROM transcription_history") == 3);
        CHECK(fs::exists(folders.root / "recordings" / "absolute.wav"));
        const auto previous = fv::backup::previous_data_dir(folders.root);
        REQUIRE(previous.has_value());
        CHECK(read_bytes(*previous / "openwhisper_cpp_settings.json") ==
              "{\"ui_theme\": \"light\"}");
        CHECK_FALSE(fs::exists(folders.root / "remote_records"));
        CHECK(tree(folders.source) == before);
    }
}

TEST_CASE("a file that cannot be hard-linked is copied", "[import]") {
    Folders folders;
    ImportOptions options = version();
    int attempts = 0;
    options.link_file = [&](const fs::path&, const fs::path&) {
        ++attempts;
        throw fs::filesystem_error("link", std::make_error_code(std::errc::cross_device_link));
    };
    const ImportResult result = import_from(folders, options);
    CHECK(attempts == 4); // two recordings, the meeting chunk and the capture
    const fs::path staged = result.stage / "data" / "recordings" / "dictation.wav";
    CHECK_FALSE(same_file(staged, folders.source / "recordings" / "dictation.wav"));
    CHECK(read_bytes(staged) == read_bytes(folders.source / "recordings" / "dictation.wav"));
    CHECK(fv::backup::apply_pending_restore(folders.root));
}

TEST_CASE("legacy JSON history is merged into an empty history table and never staged",
          "[import]") {
    fv::testkit::TempDataRoot temp;
    const fs::path base = resolved_temp(temp);
    const fs::path source = install_fixture("legacy", base / "OpenWhisper");
    const auto before = tree(source);
    const ImportResult result = stage_from_data_root(fv::legacy_identity(), source, fv::identity(),
                                                     base / "OpenWhisper_CPP");
    const fs::path data = result.stage / "data";
    const fs::path db = data / "openwhisper-cpp.db";
    CHECK(count_of(db, "SELECT COUNT(*) FROM transcription_history") == 2);
    CHECK(text_of(db, "SELECT text FROM transcription_history WHERE id='legacy-1'") ==
          "from the JSON file");
    CHECK_FALSE(fs::exists(data / "transcription_history.json"));
    CHECK_FALSE(fs::exists(data / "transcription_history.json.bak"));
    // No settings file: the staged settings are the sanitizer's and the import record.
    const Json settings = read_json(data / "openwhisper_cpp_settings.json");
    CHECK(settings.at("mcp_enabled") == false);
    CHECK(settings.at("fv_import").at("source") == fv::path_to_utf8(source));
    CHECK_FALSE(settings.contains("selected_model"));
    CHECK(tree(source) == before);
    REQUIRE(fv::backup::apply_pending_restore(base / "OpenWhisper_CPP"));
    CHECK_FALSE(fs::exists(base / "OpenWhisper_CPP" / "transcription_history.json"));
}

TEST_CASE("a running OpenWhisper refuses the import until it closes", "[import][platform]") {
    Folders folders;
    {
        const auto reader = fv::backup_test::lease_holder(folders.source, true);
        CHECK(fv::backup::openwhisper_running(folders.source));
        REQUIRE_THROWS_AS(import_from(folders), OpenWhisperRunning);
        try {
            import_from(folders);
        } catch (const OpenWhisperRunning& error) {
            CHECK(std::string(error.what()) ==
                  "OpenWhisper is running. Close OpenWhisper, then import again.");
        }
        CHECK_FALSE(fv::backup::pending_restore_exists(folders.root));
        fv::backup_test::stop(*reader);
    }
    CHECK_FALSE(fv::backup::openwhisper_running(folders.source));
    CHECK_NOTHROW(import_from(folders));
}

TEST_CASE("an import is refused while a restore is waiting for restart", "[import]") {
    Folders folders;
    const ImportResult first = import_from(folders);
    REQUIRE_THROWS_WITH(import_from(folders), "A restore is already waiting for restart");
    // The first stage is untouched and nothing else was made.
    std::vector<fs::path> stages;
    for (const auto& entry : fs::directory_iterator(folders.root / ".openwhisper-cpp-restore")) {
        if (entry.is_directory()) {
            stages.push_back(entry.path());
        }
    }
    CHECK(stages.size() == 1);
}

TEST_CASE("an import is refused while OpenWhisper's own restore waits", "[import]") {
    Folders folders;
    write_bytes(folders.source / ".openwhisper-restore" / "pending.json", "{}");
    REQUIRE_THROWS_WITH(import_from(folders),
                        ContainsSubstring("OpenWhisper has a restore waiting for its next start"));
    CHECK_FALSE(fv::backup::pending_restore_exists(folders.root));
}

TEST_CASE("a meeting OpenWhisper never ended refuses the import", "[import]") {
    Folders folders;
    {
        fv::sql::Connection connection = fv::sql::Connection::open(
            folders.source / "openwhisper.db", fv::sql::OpenMode::ReadWrite);
        connection.exec("UPDATE meeting_sessions SET status='active'");
    }
    const auto before = tree(folders.source);
    REQUIRE_THROWS_WITH(import_from(folders),
                        ContainsSubstring("OpenWhisper has a meeting that was never ended"));
    CHECK(tree(folders.source) == before);
    CHECK_FALSE(fv::backup::pending_restore_exists(folders.root));
    CHECK(fs::is_empty(folders.root / ".openwhisper-cpp-restore"));
}

TEST_CASE("a folder that is not an OpenWhisper data folder is refused", "[import]") {
    fv::testkit::TempDataRoot temp;
    REQUIRE_THROWS_WITH(stage_from_data_root(fv::legacy_identity(), temp.path() / "missing",
                                             fv::identity(), temp.path() / "OpenWhisper_CPP"),
                        ContainsSubstring("No OpenWhisper data folder was found at"));
    fs::create_directories(temp.path() / "OpenWhisper_CPP" / "inner");
    REQUIRE_THROWS_WITH(
        stage_from_data_root(fv::legacy_identity(), temp.path() / "OpenWhisper_CPP", fv::identity(),
                             temp.path() / "OpenWhisper_CPP"),
        "The OpenWhisper data folder must be separate from the OpenWhisper_CPP data "
        "folder");
}

TEST_CASE("the OpenWhisper data folders are found, read from copies", "[import]") {
    fv::testkit::TempDataRoot temp;
    const fs::path source = install_fixture("openwhisper", temp.path() / "chosen");
    const fs::path legacy = install_fixture("legacy", temp.path() / "local" / "OpenWhisper");
    const auto before = tree(source);
    const auto legacy_before = tree(legacy);
    // The per-user folder is LOCALAPPDATA on Windows and XDG_DATA_HOME on Linux: pinning only
    // the first sent this test to the real ~/.local/share/OpenWhisper of a Linux machine.
    const fv::MapEnvReader env{{"OPENWHISPER_CPP_DATA_DIR", fv::path_to_utf8(temp.path() / "fv")},
                               {"OPENWHISPER_DATA_DIR", fv::path_to_utf8(source)},
                               {"LOCALAPPDATA", fv::path_to_utf8(temp.path() / "local")},
                               {"XDG_DATA_HOME", fv::path_to_utf8(temp.path() / "local")},
                               {"HOME", fv::path_to_utf8(temp.path() / "home")},
                               {"USERPROFILE", fv::path_to_utf8(temp.path() / "home")}};
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), env);
    // On macOS the per-user folder is under HOME, where no fixture is.
    REQUIRE(paths.openwhisper_roots().size() == 2);
    if (paths.openwhisper_roots()[1] != legacy) {
        SKIP("This platform keeps the per-user folder somewhere the test does not pin.");
    }

    const auto sources = fv::backup::find_openwhisper_data(paths);

    REQUIRE(sources.size() == 2);
    CHECK(sources[0].root == source);
    CHECK(sources[0].has_settings);
    CHECK(sources[0].has_database);
    CHECK(sources[0].history_entries == 3);
    // recorded_audio.wav, the two recordings, the meeting chunk and the capture; not
    // remote_records.
    CHECK(sources[0].recordings_bytes == 18 + 60 + 52 + 43 + 12);
    CHECK(sources[1].root == legacy);
    CHECK_FALSE(sources[1].has_settings);
    CHECK(sources[1].history_entries == 2); // the JSON's, the table being empty
    CHECK(sources[1].recordings_bytes == 0);
    // SQLite read copies: neither folder gained a -wal or -shm file.
    CHECK(tree(source) == before);
    CHECK(tree(legacy) == legacy_before);

    fs::create_directories(temp.path() / "empty");
    CHECK_FALSE(fv::backup::inspect_import_source(temp.path() / "empty").has_value());
    CHECK_FALSE(fv::backup::inspect_import_source(temp.path() / "missing").has_value());
}

TEST_CASE("the imported settings map only what OpenWhisper_CPP cannot read", "[import]") {
    const auto mapped = [](const Json& legacy) {
        return fv::backup::import_settings(legacy, "C:/ow", "2026-11-15T00:00:00+00:00", "0.1.0");
    };
    CHECK(mapped(Json::parse(R"({"selected_model": "qwen_asr"})")).at("selected_model") ==
          "parakeet");
    CHECK(mapped(Json::parse(R"({"selected_model": 3})")).at("selected_model") == "parakeet");
    CHECK(mapped(Json::parse(R"({"selected_model": "api"})")).at("selected_model") == "api");
    CHECK(mapped(Json::parse(R"({"selected_model": "remote"})")).at("selected_model") == "remote");
    // An alias of the API engine stays as stored: the API model is resolved from its name.
    CHECK(mapped(Json::parse(R"({"selected_model": "api_gpt4o"})")).at("selected_model") ==
          "api_gpt4o");
    CHECK_FALSE(mapped(Json::object()).contains("selected_model"));
    const Json record = mapped(Json::object()).at("fv_import");
    CHECK(record == Json::parse(R"({"source": "C:/ow", "at": "2026-11-15T00:00:00+00:00",
                                    "version": "0.1.0"})"));
}

TEST_CASE("the coordinator stages an import as a restore", "[import]") {
    fv::testkit::TempDataRoot temp;
    fv::FakeClock clock;
    fv::ManualExecutor ui(clock);
    fv::ManualExecutor worker;
    fv::backup::testing::FakeBackupService service;
    fv::backup::testing::FakeHooks hooks;
    fv::MapEnvReader env{{"OPENWHISPER_CPP_DATA_DIR", fv::path_to_utf8(temp.path() / "data")}};
    fv::pin_per_user_folders(env, temp.path() / "per-user");
    fv::backup::BackupCoordinator coordinator(fv::Paths::resolve(fv::identity(), env),
                                              fv::identity(), hooks.hooks(), ui, worker, clock,
                                              "0.1.0", &service);
    std::vector<std::pair<std::string, std::string>> staged;
    auto subscription = coordinator.restore_staged.connect(
        ui, [&](const std::string& stage, const std::string& error) {
            staged.emplace_back(stage, error);
        });
    const auto drain = [&] {
        while (worker.run_all() + ui.run_all() > 0) {
        }
    };
    const fs::path source = temp.path() / "OpenWhisper";

    SECTION("the stage on success") {
        std::optional<ImportOptions> seen;
        service.on_import = [&](const fs::path& folder, const ImportOptions& options) {
            seen = options;
            ImportResult result;
            result.stage = folder / "stage";
            return result;
        };
        coordinator.prepare_import(source, true);
        CHECK(coordinator.busy());
        CHECK(coordinator.operation() == "restore");
        drain();
        CHECK(service.calls() ==
              std::vector<std::vector<std::string>>{{"import", fv::path_to_utf8(source), "true"}});
        REQUIRE(seen.has_value());
        CHECK(seen->copy_credentials);
        CHECK(seen->app_version == "0.1.0");
        CHECK(staged == std::vector<std::pair<std::string, std::string>>{
                            {fv::path_to_utf8(source / "stage"), ""}});
        CHECK(hooks.events() ==
              std::vector<std::string>{"acquire", "context_enter", "context_exit", "release"});
        CHECK_FALSE(coordinator.busy());
    }

    SECTION("the refusal as the error") {
        service.on_import = [](const fs::path&, const ImportOptions&) -> ImportResult {
            throw OpenWhisperRunning("OpenWhisper is running. Close OpenWhisper, then import "
                                     "again.");
        };
        coordinator.prepare_import(source, false);
        drain();
        CHECK(staged == std::vector<std::pair<std::string, std::string>>{
                            {"", "OpenWhisper is running. Close OpenWhisper, then import again."}});
    }

    SECTION("a reservation refusal never starts it") {
        hooks.set_refusal("Finish the recording first.");
        coordinator.prepare_import(source, false);
        drain();
        CHECK(service.calls().empty());
        CHECK(staged == std::vector<std::pair<std::string, std::string>>{
                            {"", "Finish the recording first."}});
    }
}
