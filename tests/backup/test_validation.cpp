// Every rule a bundle is inspected against (services/backup.py:852-1062), one crafted bundle per
// rule, each refused with OpenWhisper's text.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "archive/zip_writer.h"
#include "backup/backup.h"
#include "backup/inspect.h"
#include "backup/names.h"
#include "backup/testing/sample_data.h"
#include "base/cancel.h"
#include "base/hash.h"
#include "base/identity.h"
#include "db/schema.h"
#include "pyport/json.h"
#include "sql/db.h"
#include "testkit/testkit.h"

namespace fs = std::filesystem;
using fv::Json;
using fv::archive::Method;
using fv::archive::ZipWriter;
using fv::backup::BackupError;
using fv::backup::inspect_backup;
using fv::backup::testing::read_bytes;
using fv::backup::testing::write_bytes;

namespace {

constexpr std::uint32_t kRegularFileAttr = (0100000u | 0600u) << 16;

// A history audio path that is absolute here, as pathlib reads it.
#ifdef _WIN32
constexpr const char* kAbsoluteAudio = "C:\\x.wav";
#else
constexpr const char* kAbsoluteAudio = "/x.wav";
#endif

const fv::backup::Names& names() {
    return fv::backup::names_for(fv::identity());
}

struct Member {
    std::string name;
    std::string data;
    Method method = Method::Deflated;
    std::uint32_t attr = kRegularFileAttr;
};

struct Bundle {
    std::vector<Member> members;
    Json manifest;
    // The manifest member's bytes when they are not the manifest's compact JSON.
    std::optional<std::string> manifest_text;
    bool with_manifest = true;
    // Sizes the central directory declares for one member instead of the real ones.
    std::string patched_member;
    std::optional<std::uint64_t> declared_size;
    std::optional<std::uint64_t> declared_compressed;
};

Json listing(const std::vector<Member>& members) {
    Json files = Json::array();
    for (const auto& member : members) {
        files.push_back(Json{{"path", member.name},
                             {"size", member.data.size()},
                             {"sha256", fv::sha256_hex(member.data)}});
    }
    return files;
}

// The manifest's file list made again from the members.
void relist(Bundle& bundle) {
    bundle.manifest["files"] = listing(bundle.members);
}

// A database file's bytes after running `sql` on a fresh one.
std::string database_bytes(const fs::path& folder, const std::string& sql) {
    static int counter = 0;
    const fs::path path = folder / ("database-" + std::to_string(++counter) + ".db");
    {
        fv::sql::Connection connection = fv::sql::Connection::open(path, fv::sql::OpenMode::Create);
        connection.exec(sql);
    }
    return read_bytes(path);
}

std::string schema_sql(int version = fv::schema::schema_version) {
    return "CREATE TABLE schema_version (version INTEGER PRIMARY KEY);"
           "INSERT INTO schema_version VALUES (" +
           std::to_string(version) + ");";
}

Bundle valid_bundle(const fs::path& folder) {
    Bundle bundle;
    bundle.members = {
        {names().settings_member, R"({"ui_theme": "dark"})"},
        {names().database_member, database_bytes(folder, schema_sql())},
        {"data/scratchpad.txt", "notes"},
    };
    bundle.manifest = Json{{"format", "openwhisper-cpp-backup"},
                           {"format_version", 1},
                           {"created_at", "2026-11-15T00:00:00+00:00"},
                           {"app_version", "0.1.0"},
                           {"schema_version", fv::schema::schema_version},
                           {"include_recordings", true},
                           {"files", listing(bundle.members)},
                           {"warnings", Json::array()}};
    return bundle;
}

std::uint32_t u32(const std::string& bytes, std::size_t at) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

std::uint16_t u16(const std::string& bytes, std::size_t at) {
    std::uint16_t value = 0;
    std::memcpy(&value, bytes.data() + at, 2);
    return value;
}

// Rewrites the sizes the central directory gives `name`: in the Zip64 extra field when the
// 32-bit field defers to it (uncompressed size first, then compressed), else in place.
void declare_sizes(std::string& zip, const std::string& name, std::optional<std::uint64_t> size,
                   std::optional<std::uint64_t> compressed) {
    const std::string signature("PK\x01\x02", 4);
    for (std::size_t at = zip.find(signature); at != std::string::npos;
         at = zip.find(signature, at + 4)) {
        const std::uint16_t name_length = u16(zip, at + 28);
        if (zip.compare(at + 46, name_length, name) != 0) {
            continue;
        }
        const bool size_in_extra = u32(zip, at + 24) == 0xFFFFFFFF;
        const bool compressed_in_extra = u32(zip, at + 20) == 0xFFFFFFFF;
        std::size_t extra = at + 46 + name_length;
        const std::size_t extra_end = extra + u16(zip, at + 30);
        while (extra + 4 <= extra_end && u16(zip, extra) != 1) {
            extra += 4 + u16(zip, extra + 2);
        }
        std::size_t field = extra + 4;
        auto put = [&](std::size_t offset, std::uint64_t value, std::size_t width) {
            std::memcpy(zip.data() + offset, &value, width);
        };
        if (size_in_extra) {
            if (size) {
                put(field, *size, 8);
            }
            field += 8;
        } else if (size) {
            put(at + 24, *size, 4);
        }
        if (compressed_in_extra) {
            if (compressed) {
                put(field, *compressed, 8);
            }
        } else if (compressed) {
            put(at + 20, *compressed, 4);
        }
        return;
    }
    FAIL("no central directory entry named " << name);
}

void write_bundle(const fs::path& path, const Bundle& bundle) {
    {
        ZipWriter writer = ZipWriter::create(path);
        for (const auto& member : bundle.members) {
            writer.add_bytes(member.name, std::as_bytes(std::span(member.data)), member.method,
                             member.attr);
        }
        if (bundle.with_manifest) {
            fv::py::DumpOptions compact;
            compact.ensure_ascii = false;
            compact.item_sep = ",";
            compact.key_sep = ":";
            const std::string text =
                bundle.manifest_text.value_or(fv::py::dumps(bundle.manifest, compact));
            writer.add_bytes("manifest.json", std::as_bytes(std::span(text)), Method::Deflated,
                             kRegularFileAttr);
        }
        writer.close();
    }
    if (!bundle.patched_member.empty()) {
        std::string bytes = read_bytes(path);
        declare_sizes(bytes, bundle.patched_member, bundle.declared_size,
                      bundle.declared_compressed);
        write_bytes(path, bytes);
    }
}

Member* member(Bundle& bundle, std::string_view name) {
    for (auto& item : bundle.members) {
        if (item.name == name) {
            return &item;
        }
    }
    return nullptr;
}

struct Rule {
    const char* what;
    std::function<void(Bundle&, const fs::path&)> craft;
    const char* message; // nullptr: the bundle is accepted
};

} // namespace

TEST_CASE("every rule of a bundle refuses with OpenWhisper's text", "[backup]") {
    fv::testkit::TempDataRoot temp;
    const std::string marker = "__OW_BACKUP_DATA__/";
    const std::vector<Rule> rules = {
        {"a well-formed bundle", [](Bundle&, const fs::path&) {}, nullptr},
        {"names that differ only in case",
         [](Bundle& b, const fs::path&) {
             b.members.push_back({"data/recordings/A.wav", "a", Method::Stored});
             b.members.push_back({"data/recordings/a.wav", "b", Method::Stored});
             relist(b);
         },
         "Archive contains duplicate names or directories"},
        {"a file outside the owned names",
         [](Bundle& b, const fs::path&) {
             b.members.push_back({"data/notes.txt", "x"});
             relist(b);
         },
         "Archive contains a file outside owned data"},
        {"the raw legacy history",
         [](Bundle& b, const fs::path&) {
             b.members.push_back({"data/transcription_history.json", "{}"});
             relist(b);
         },
         "Archive contains transient or raw legacy data"},
        {"a file nested below a file name",
         [](Bundle& b, const fs::path&) {
             b.members.push_back({"data/scratchpad.txt/x", "x"});
             relist(b);
         },
         "Archive contains an unexpected nested file"},
        {"a member name with a colon",
         [](Bundle& b, const fs::path&) {
             b.members.push_back({"data/recordings/a:b.wav", "x"});
             relist(b);
         },
         "Unsafe archive member name"},
        {"a named pipe",
         [](Bundle& b, const fs::path&) {
             b.members.push_back(
                 {"data/recordings/fifo", "", Method::Stored, (0010000u | 0600u) << 16});
             relist(b);
         },
         "Archive contains a symbolic link or special file"},
        {"permission bits without a file type",
         [](Bundle& b, const fs::path&) {
             b.members.push_back({"data/recordings/plain.wav", "x", Method::Stored, 0600u << 16});
             relist(b);
         },
         nullptr},
        {"a member declared over 64 GiB",
         [](Bundle& b, const fs::path&) {
             b.patched_member = "data/scratchpad.txt";
             b.declared_size = (std::uint64_t{1} << 36) + 1;
         },
         "Archive member exceeds size limit"},
        {"a member that would expand more than 10,000 times",
         [](Bundle& b, const fs::path&) {
             member(b, "data/scratchpad.txt")->data = std::string(20'001, 'n');
             relist(b);
             b.patched_member = "data/scratchpad.txt";
             b.declared_compressed = 2;
         },
         "Archive has an unsafe compression ratio"},
        {"a member with content and no compressed bytes",
         [](Bundle& b, const fs::path&) {
             b.patched_member = "data/scratchpad.txt";
             b.declared_compressed = 0;
         },
         "Archive has an unsafe compression ratio"},
        {"no manifest", [](Bundle& b, const fs::path&) { b.with_manifest = false; },
         "Archive manifest is missing or too large"},
        {"a manifest that is not JSON", [](Bundle& b, const fs::path&) { b.manifest_text = "{"; },
         "Archive manifest is invalid"},
        {"a manifest that is not UTF-8",
         [](Bundle& b, const fs::path&) { b.manifest_text = "{\"a\": \"\xff\"}"; },
         "Archive manifest is invalid"},
        {"a manifest that is a list", [](Bundle& b, const fs::path&) { b.manifest_text = "[]"; },
         "Unsupported backup format"},
        {"format version 2", [](Bundle& b, const fs::path&) { b.manifest["format_version"] = 2; },
         "Unsupported backup format"},
        {"format version 1.0, which Python compares equal to 1",
         [](Bundle& b, const fs::path&) { b.manifest["format_version"] = 1.0; }, nullptr},
        {"a creation time that is not text",
         [](Bundle& b, const fs::path&) { b.manifest["created_at"] = 5; },
         "Archive metadata is invalid"},
        {"no app version", [](Bundle& b, const fs::path&) { b.manifest.erase("app_version"); },
         "Archive metadata is invalid"},
        {"a schema newer than this app",
         [](Bundle& b, const fs::path&) {
             b.manifest["schema_version"] = fv::schema::schema_version + 1;
         },
         "Backup database schema is incompatible with this app"},
        {"a schema version that is a bool",
         [](Bundle& b, const fs::path&) { b.manifest["schema_version"] = true; },
         "Backup database schema is incompatible with this app"},
        {"a negative schema version",
         [](Bundle& b, const fs::path&) { b.manifest["schema_version"] = -1; },
         "Backup database schema is incompatible with this app"},
        {"a recording flag that is a number",
         [](Bundle& b, const fs::path&) { b.manifest["include_recordings"] = 1; },
         "Archive recording flag is invalid"},
        {"a file list shorter than the members",
         [](Bundle& b, const fs::path&) { b.manifest["files"].erase(2); },
         "Archive file list does not match its members"},
        {"a file list that is not a list",
         [](Bundle& b, const fs::path&) { b.manifest["files"] = Json::object(); },
         "Archive file list does not match its members"},
        {"a listed file that is not an object",
         [](Bundle& b, const fs::path&) { b.manifest["files"][2] = "data/scratchpad.txt"; },
         "Archive file metadata is invalid"},
        {"a listed path that is unsafe",
         [](Bundle& b, const fs::path&) { b.manifest["files"][2]["path"] = "data/../x"; },
         "Unsafe archive member name"},
        {"a file listed twice",
         [](Bundle& b, const fs::path&) { b.manifest["files"][2] = b.manifest["files"][1]; },
         "Archive file metadata is invalid"},
        {"a listed size that differs from the member",
         [](Bundle& b, const fs::path&) { b.manifest["files"][2]["size"] = 4; },
         "Archive file metadata is invalid"},
        {"a listed size that is a bool",
         [](Bundle& b, const fs::path&) {
             member(b, "data/scratchpad.txt")->data = "n";
             relist(b);
             b.manifest["files"][2]["size"] = true;
         },
         "Archive file metadata is invalid"},
        {"a checksum in upper case",
         [](Bundle& b, const fs::path&) {
             std::string digest = b.manifest["files"][2]["sha256"].get<std::string>();
             for (char& c : digest) {
                 c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
             }
             b.manifest["files"][2]["sha256"] = digest;
         },
         "Archive file metadata is invalid"},
        {"no settings member",
         [](Bundle& b, const fs::path&) {
             b.members.erase(b.members.begin());
             relist(b);
         },
         "Archive is missing core data"},
        {"recordings in a text-only bundle",
         [](Bundle& b, const fs::path&) {
             b.members.push_back({"data/recordings/a.wav", "RIFF", Method::Stored});
             relist(b);
             b.manifest["include_recordings"] = false;
         },
         "Text-only archive contains recording assets"},
        {"a warning over 500 characters",
         [](Bundle& b, const fs::path&) {
             b.manifest["warnings"] = Json::array({std::string(501, 'w')});
         },
         "Archive warnings are invalid"},
        {"a warning of 500 characters in 1,000 bytes",
         [](Bundle& b, const fs::path&) {
             std::string warning;
             for (int i = 0; i < 500; ++i) {
                 warning += "\xc3\xa9";
             }
             b.manifest["warnings"] = Json::array({warning});
         },
         nullptr},
        {"warnings that are not text",
         [](Bundle& b, const fs::path&) { b.manifest["warnings"] = Json::array({5}); },
         "Archive warnings are invalid"},
        {"no warnings at all", [](Bundle& b, const fs::path&) { b.manifest.erase("warnings"); },
         nullptr},
        {"warnings that are null",
         [](Bundle& b, const fs::path&) { b.manifest["warnings"] = nullptr; },
         "Archive warnings are invalid"},
        {"a settings member over 8 MiB",
         [](Bundle& b, const fs::path&) {
             member(b, names().settings_member)->data = std::string((8u << 20) + 1, ' ') + "{}";
             relist(b);
         },
         "Settings file is too large"},
        {"a checksum of other bytes",
         [](Bundle& b, const fs::path&) {
             b.manifest["files"][2]["sha256"] = fv::sha256_hex("other");
         },
         "Archive checksum mismatch: data/scratchpad.txt"},
        {"settings that are a list",
         [](Bundle& b, const fs::path&) {
             member(b, names().settings_member)->data = "[]";
             relist(b);
         },
         "Backup settings JSON must be an object"},
        {"settings that are not JSON",
         [](Bundle& b, const fs::path&) {
             member(b, names().settings_member)->data = "{";
             relist(b);
         },
         "Backup settings JSON is invalid"},
        {"a database of another schema than the manifest says",
         [](Bundle& b, const fs::path& folder) {
             member(b, names().database_member)->data =
                 database_bytes(folder, schema_sql(fv::schema::schema_version - 1));
             relist(b);
         },
         "Backup database metadata or meeting state is invalid"},
        {"a live meeting",
         [&](Bundle& b, const fs::path& folder) {
             member(b, names().database_member)->data = database_bytes(
                 folder, schema_sql() +
                             "CREATE TABLE meeting_sessions (id TEXT, status TEXT, spool_dir TEXT);"
                             "INSERT INTO meeting_sessions VALUES ('m', 'paused', '" +
                             marker + "meetings/m');");
             relist(b);
         },
         "Backup database metadata or meeting state is invalid"},
        {"a meeting spool folder outside the marker",
         [](Bundle& b, const fs::path& folder) {
             member(b, names().database_member)->data = database_bytes(
                 folder, schema_sql() +
                             "CREATE TABLE meeting_sessions (id TEXT, status TEXT, spool_dir TEXT);"
                             "INSERT INTO meeting_sessions VALUES ('m', 'ended', 'C:/m');");
             relist(b);
         },
         "Backup database has an unsafe data path"},
        {"a meeting spool folder outside meetings",
         [&](Bundle& b, const fs::path& folder) {
             member(b, names().database_member)->data = database_bytes(
                 folder, schema_sql() +
                             "CREATE TABLE meeting_sessions (id TEXT, status TEXT, spool_dir TEXT);"
                             "INSERT INTO meeting_sessions VALUES ('m', 'ended', '" +
                             marker + "recordings/m');");
             relist(b);
         },
         "Backup database has an unsafe meeting path"},
        {"a meeting chunk the bundle lacks",
         [&](Bundle& b, const fs::path& folder) {
             member(b, names().database_member)->data = database_bytes(
                 folder, schema_sql() +
                             "CREATE TABLE meeting_audio_chunks (id INTEGER, file_path TEXT);"
                             "INSERT INTO meeting_audio_chunks VALUES (1, '" +
                             marker + "meetings/m/mic-000.wav');");
             relist(b);
         },
         "Backup database references missing meeting audio"},
        {"a meeting chunk the bundle has",
         [&](Bundle& b, const fs::path& folder) {
             member(b, names().database_member)->data = database_bytes(
                 folder, schema_sql() +
                             "CREATE TABLE meeting_audio_chunks (id INTEGER, file_path TEXT);"
                             "INSERT INTO meeting_audio_chunks VALUES (1, '" +
                             marker + "meetings/m/mic-000.wav');");
             b.members.push_back({"data/meetings/m/mic-000.wav", "RIFF", Method::Stored});
             relist(b);
         },
         nullptr},
        {"history audio at an absolute path",
         [](Bundle& b, const fs::path& folder) {
             member(b, names().database_member)->data = database_bytes(
                 folder, schema_sql() +
                             "CREATE TABLE transcription_history (id TEXT, audio_file TEXT);"
                             "INSERT INTO transcription_history VALUES ('h', '" +
                             kAbsoluteAudio + "');");
             relist(b);
         },
         "Backup database has an unsafe recording path"},
        {"history audio that climbs out of recordings",
         [](Bundle& b, const fs::path& folder) {
             member(b, names().database_member)->data = database_bytes(
                 folder, schema_sql() +
                             "CREATE TABLE transcription_history (id TEXT, audio_file TEXT);"
                             "INSERT INTO transcription_history VALUES ('h', '../x.wav');");
             relist(b);
         },
         "Unsafe archive member name"},
        {"history audio the bundle lacks",
         [](Bundle& b, const fs::path& folder) {
             member(b, names().database_member)->data = database_bytes(
                 folder, schema_sql() +
                             "CREATE TABLE transcription_history (id TEXT, audio_file TEXT);"
                             "INSERT INTO transcription_history VALUES ('h', 'gone.wav');");
             relist(b);
         },
         "Backup database references missing recording audio"},
    };

    int index = 0;
    for (const auto& rule : rules) {
        DYNAMIC_SECTION(rule.what) {
            const fs::path folder = temp.path() / std::to_string(++index);
            fs::create_directories(folder);
            Bundle bundle = valid_bundle(folder);
            rule.craft(bundle, folder);
            const fs::path archive = folder / "crafted.fvbackup";
            write_bundle(archive, bundle);
            if (rule.message == nullptr) {
                const fv::backup::BackupInfo info = inspect_backup(archive);
                CHECK(info.file_count == bundle.manifest["files"].size());
                CHECK_FALSE(info.legacy_format);
            } else {
                CHECK_THROWS_MATCHES(inspect_backup(archive), BackupError,
                                     Catch::Matchers::Message(rule.message));
            }
        }
    }
}

TEST_CASE("a database member that is not SQLite fails as SQLite's own error", "[backup]") {
    // _quick_check raises sqlite3.DatabaseError, which inspect_backup lets through.
    fv::testkit::TempDataRoot temp;
    Bundle bundle = valid_bundle(temp.path());
    member(bundle, names().database_member)->data = "not a database at all, just text";
    relist(bundle);
    const fs::path archive = temp.path() / "garbage.fvbackup";
    write_bundle(archive, bundle);
    CHECK_THROWS_AS(inspect_backup(archive), fv::sql::Error);
}

TEST_CASE("inspecting stops when cancelled", "[backup]") {
    fv::testkit::TempDataRoot temp;
    const Bundle bundle = valid_bundle(temp.path());
    const fs::path archive = temp.path() / "valid.fvbackup";
    write_bundle(archive, bundle);
    fv::CancelSource cancel;
    cancel.cancel();
    CHECK_THROWS_AS(inspect_backup(archive, cancel.token()), fv::backup::BackupCancelled);
    CHECK(inspect_backup(archive).file_count == 3);
}
