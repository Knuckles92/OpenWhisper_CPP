// The restore's small JSON files: services/backup.py's _write_json_atomic and _read_json.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <filesystem>
#include <string>

#include "backup/backup.h"
#include "backup/detail/json_file.h"
#include "backup/testing/sample_data.h"
#include "base/utf8.h"
#include "testkit/testkit.h"

namespace fs = std::filesystem;
using Catch::Matchers::Message;
using fv::Json;
using fv::backup::BackupError;
using fv::backup::detail::read_json;
using fv::backup::detail::write_json_atomic;
using fv::backup::testing::read_bytes;
using fv::backup::testing::write_bytes;

#ifdef _WIN32
constexpr const char* kNewline = "\r\n";
#else
constexpr const char* kNewline = "\n";
#endif

TEST_CASE("restore metadata is written compact and whole", "[backup]") {
    fv::testkit::TempDataRoot temp;
    const fs::path home = temp.path() / "restore-home";
    const fs::path pending = home / "pending.json";
    const Json value = Json{
        {"stage", "stage-0123"}, {"note", "caf\xc3\xa9\nline"}, {"files", Json::array({1, 2})}};
    write_json_atomic(pending, value); // the folder is made first
    CHECK(read_bytes(pending) == std::string(R"({"stage":"stage-0123","note":"caf)"
                                             "\xc3\xa9"
                                             R"(\nline","files":[1,2]})") +
                                     kNewline);
    CHECK(read_json(pending) == value);

    write_json_atomic(pending, Json{{"phase", "applied"}});
    CHECK(read_json(pending) == Json{{"phase", "applied"}});
    // Only the file itself is left: no temporary beside it.
    int entries = 0;
    for (const auto& entry : fs::directory_iterator(home)) {
        CHECK(fv::path_to_utf8(entry.path().filename()) == "pending.json");
        ++entries;
    }
    CHECK(entries == 1);
}

TEST_CASE("restore metadata that is not a small JSON object is refused", "[backup]") {
    fv::testkit::TempDataRoot temp;
    const fs::path folder = temp.path();
    CHECK_THROWS_MATCHES(read_json(folder / "missing.json"), BackupError,
                         Message("Restore metadata is missing or invalid"));
    fs::create_directories(folder / "a-folder.json");
    CHECK_THROWS_MATCHES(read_json(folder / "a-folder.json"), BackupError,
                         Message("Restore metadata is missing or invalid"));
    write_bytes(folder / "large.json", R"({"key": "value"})");
    CHECK(read_json(folder / "large.json", 16) == Json{{"key", "value"}});
    CHECK_THROWS_MATCHES(read_json(folder / "large.json", 15), BackupError,
                         Message("Restore metadata is missing or invalid"));
    write_bytes(folder / "broken.json", R"({"key": )");
    CHECK_THROWS_MATCHES(read_json(folder / "broken.json"), BackupError,
                         Message("Restore metadata is invalid"));
    write_bytes(folder / "latin1.json", "{\"key\": \"caf\xe9\"}");
    CHECK_THROWS_MATCHES(read_json(folder / "latin1.json"), BackupError,
                         Message("Restore metadata is invalid"));
    write_bytes(folder / "bom.json", "\xef\xbb\xbf{}");
    CHECK_THROWS_MATCHES(read_json(folder / "bom.json"), BackupError,
                         Message("Restore metadata is invalid"));
    write_bytes(folder / "list.json", "[1, 2]");
    CHECK_THROWS_MATCHES(read_json(folder / "list.json"), BackupError,
                         Message("Restore metadata must be an object"));
}
