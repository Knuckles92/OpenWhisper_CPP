// The OpenWhisper import's open items (sweep I2): discovery without a database copy, the saved
// keys of speech and local servers, the identities a copy is made between, an unreadable store,
// and OpenWhisper's settings or legacy history that cannot be read. FioriVoce only.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "backup/backup.h"
#include "backup/import.h"
#include "backup/import_credentials.h"
#include "backup/testing/sample_data.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "pyport/json.h"
#include "sql/db.h"
#include "testkit/testkit.h"

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::backup::copy_credentials;
using fv::backup::CopyReport;
using fv::backup::legacy_credential_names;
using fv::backup::testing::read_bytes;
using fv::backup::testing::write_bytes;

namespace {

fs::path fixture(std::string_view name) {
    return fv::testkit::source_root() / "tests" / "fixtures" / "import" / std::string(name);
}

fs::path copy_fixture(std::string_view name, const fs::path& target) {
    fs::create_directories(target.parent_path());
    fs::copy(fixture(name), target, fs::copy_options::recursive);
    return target;
}

// Every entry below `root`, by name.
std::vector<std::string> names_below(const fs::path& root) {
    std::vector<std::string> names;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        names.push_back(fv::path_to_utf8(entry.path().lexically_relative(root)));
    }
    std::ranges::sort(names);
    return names;
}

// Points the temporary folder of this process somewhere that does not exist while it lives, so
// that anything that copies there fails.
class NoTemporaryFolder {
public:
    explicit NoTemporaryFolder(const fs::path& missing) {
        for (const char* name : kNames) {
            saved_[name] = fv::testkit::get_env(name);
            fv::testkit::set_env(name, fv::path_to_utf8(missing));
        }
    }
    ~NoTemporaryFolder() {
        for (const auto& [name, value] : saved_) {
            fv::testkit::set_env(name, value);
        }
    }
    NoTemporaryFolder(const NoTemporaryFolder&) = delete;
    NoTemporaryFolder& operator=(const NoTemporaryFolder&) = delete;

private:
    static constexpr const char* kNames[] = {"TMPDIR", "TMP", "TEMP", "TEMPDIR"};
    std::map<std::string, std::optional<std::string>> saved_;
};

// A store that cannot be used, as a locked keyring or a missing Credential Manager reads.
class UnusableBackend final : public fv::MemoryCredentialBackend {
public:
    fv::CredentialStatus probe() override { return {false, "unusable store", "locked"}; }
};

struct ServiceIdentity {
    ServiceIdentity(const fv::AppIdentity& base, std::string service_name, std::string prefix = {})
        : service(std::move(service_name)), env_prefix(std::move(prefix)), identity(base) {
        identity.credential_service = service;
        if (!env_prefix.empty()) {
            identity.env_prefix = env_prefix;
        }
    }
    std::string service;
    std::string env_prefix;
    fv::AppIdentity identity;
};

} // namespace

TEST_CASE("discovery counts the history in place, without copying the database", "[import]") {
    fv::testkit::TempDataRoot temp;
    const fs::path source = copy_fixture("openwhisper", temp.path() / "OpenWhisper");
    SECTION("no write-ahead log") {}
    SECTION("an empty write-ahead log, as a checkpoint leaves it") {
        write_bytes(source / "openwhisper.db-wal", "");
    }
    const auto before = names_below(source);
    // Without the fix the database went to a temporary folder first: none can be made here.
    NoTemporaryFolder no_temp(temp.path() / "no-temporary-folder");

    const auto found = fv::backup::inspect_import_source(source);

    REQUIRE(found.has_value());
    CHECK(found->history_entries == 3);
    CHECK(names_below(source) == before); // no -wal or -shm beside it
}

TEST_CASE("discovery counts rows still in OpenWhisper's -wal", "[import]") {
    fv::testkit::TempDataRoot temp;
    const fs::path source = copy_fixture("openwhisper", temp.path() / "OpenWhisper");
    // A running OpenWhisper: a connection that keeps its last row in the write-ahead log.
    fv::sql::Connection writer =
        fv::sql::Connection::open(source / "openwhisper.db", fv::sql::OpenMode::ReadWrite);
    writer.exec("PRAGMA journal_mode=WAL");
    writer.exec("PRAGMA wal_autocheckpoint=0");
    writer.exec("INSERT INTO transcription_history (id, text, timestamp, model) "
                "VALUES ('in-the-wal', 'not yet checkpointed', '2026-11-19T09:00:00', 'parakeet')");
    REQUIRE(fs::exists(source / "openwhisper.db-wal"));

    const auto found = fv::backup::inspect_import_source(source);

    REQUIRE(found.has_value());
    CHECK(found->history_entries == 4);
}

TEST_CASE("the keys of speech servers and local text servers are imported too", "[import]") {
    const Json settings = Json::parse(R"({
        "api_transcription_servers": {
            "custom": {"base_url": "http://10.0.0.2:9000/v1", "api_key_env": " CUSTOM_STT_KEY "},
            "speaches": {"api_key_env": "SPEACHES_KEY"},
            "groq": {"api_key_env": "HOSTED_IS_NOT_READ"},
            "whisper_cpp": {"api_key_env": "lower_case"},
            "vllm": "not an object"
        },
        "ollama_api_key_env": "OLLAMA_KEY",
        "llama_cpp_api_key_env": "SPEACHES_KEY",
        "localai_api_key_env": 7,
        "lm_studio_base_url": "http://127.0.0.1:1234/v1"
    })");

    const auto names = legacy_credential_names(settings);

    const std::vector<std::string> tail(names.end() - 3, names.end());
    // Speech servers in OpenWhisper's display order, then local servers, without repeats.
    CHECK(tail == std::vector<std::string>{"SPEACHES_KEY", "CUSTOM_STT_KEY", "OLLAMA_KEY"});
    CHECK(names.size() == 11 + 3);
}

TEST_CASE("a copy names keys by the identities it is made between", "[import]") {
    auto backend = std::make_shared<fv::MemoryCredentialBackend>();
    const ServiceIdentity from_app(fv::legacy_identity(), "FromService", "OLDAPP_");
    const ServiceIdentity to_app(fv::identity(), "ToService", "NEWAPP_");
    fv::CredentialStore from(backend, from_app.identity);
    fv::CredentialStore to(backend, to_app.identity);
    from.set("OLDAPP_ACTION_SECRET_a1", "action-secret-0123");
    from.set("OPENWHISPER_ACTION_SECRET_a1", "wrong-prefix-0123");
    const Json settings = Json::parse(R"({"actions": [{"id": "a1"}]})");

    CHECK(legacy_credential_names(settings, from_app.identity).back() == "OLDAPP_ACTION_SECRET_a1");
    const CopyReport report =
        copy_credentials(from, to, settings, from_app.identity, to_app.identity);

    CHECK(report.copied == std::vector<std::string>{"NEWAPP_ACTION_SECRET_a1"});
    CHECK(to.get("NEWAPP_ACTION_SECRET_a1") == "action-secret-0123");
    CHECK_FALSE(to.get("FIORIVOCE_ACTION_SECRET_a1").has_value());
}

TEST_CASE("a store that cannot be read fails every name instead of skipping it", "[import]") {
    auto unusable = std::make_shared<UnusableBackend>();
    auto own = std::make_shared<fv::MemoryCredentialBackend>();
    fv::CredentialStore from(unusable, fv::legacy_identity());
    fv::CredentialStore to(own, fv::identity());

    const CopyReport report = copy_credentials(from, to, Json::object());

    CHECK(report.unreadable);
    CHECK(report.skipped.empty());
    CHECK(report.failed == legacy_credential_names(Json::object()));
    CHECK(report.copied.empty());

    SECTION("an import says so once") {
        fv::testkit::TempDataRoot temp;
        const fs::path source = copy_fixture("legacy", temp.path() / "OpenWhisper");
        fv::backup::ImportOptions options;
        options.copy_credentials = true;
        options.credentials_from = &from;
        options.credentials_to = &to;
        const auto result = fv::backup::stage_from_data_root(
            fv::legacy_identity(), source, fv::identity(), temp.path() / "FioriVoce", options);
        CHECK(std::ranges::count(result.warnings, "Could not read the keys OpenWhisper saved") ==
              1);
        CHECK(std::ranges::none_of(result.warnings, [](const std::string& warning) {
            return warning.starts_with("Could not copy the saved key");
        }));
    }
}

TEST_CASE("settings OpenWhisper cannot read import as its defaults", "[import]") {
    fv::testkit::TempDataRoot temp;
    // The legacy fixture: its database holds no paths that would need rebasing to stage.
    const fs::path source = copy_fixture("legacy", temp.path() / "OpenWhisper");

    SECTION("not JSON") {
        write_bytes(source / "openwhisper_settings.json", "{\"selected_model\": ");
    }
    SECTION("not an object") {
        write_bytes(source / "openwhisper_settings.json", "[1, 2]");
    }
    SECTION("not UTF-8") {
        write_bytes(source / "openwhisper_settings.json", "{\"a\": \"\xff\"}");
    }

    const auto result = fv::backup::stage_from_data_root(fv::legacy_identity(), source,
                                                         fv::identity(), temp.path() / "FioriVoce");

    REQUIRE_FALSE(result.warnings.empty());
    CHECK(result.warnings.front() ==
          "OpenWhisper's settings could not be read; the default settings were imported");
    const Json staged =
        fv::py::loads(read_bytes(result.stage / "data" / "fiorivoce_settings.json"));
    CHECK(staged.contains("fv_import"));
    CHECK_FALSE(staged.contains("selected_model"));
    // The history comes over all the same.
    fv::sql::Connection staged_db = fv::sql::Connection::open(
        result.stage / "data" / "fiorivoce.db", fv::sql::OpenMode::ReadOnly);
    CHECK(std::get<std::int64_t>(staged_db.scalar("SELECT COUNT(*) FROM transcription_history")) ==
          2);
}

TEST_CASE("a folder in place of OpenWhisper's settings still refuses the import", "[import]") {
    fv::testkit::TempDataRoot temp;
    const fs::path source = copy_fixture("legacy", temp.path() / "OpenWhisper");
    fs::create_directories(source / "openwhisper_settings.json");

    CHECK_THROWS_AS(fv::backup::stage_from_data_root(fv::legacy_identity(), source, fv::identity(),
                                                     temp.path() / "FioriVoce"),
                    fv::backup::BackupError);
}

TEST_CASE("a legacy history OpenWhisper cannot read is left out of the import", "[import]") {
    fv::testkit::TempDataRoot temp;
    const fs::path source = copy_fixture("legacy", temp.path() / "OpenWhisper");
    write_bytes(source / "transcription_history.json", "{\"entries\": \"not a list\"}");

    const auto result = fv::backup::stage_from_data_root(fv::legacy_identity(), source,
                                                         fv::identity(), temp.path() / "FioriVoce");

    CHECK(std::ranges::any_of(result.warnings, [](const std::string& warning) {
        return warning.starts_with("Could not merge transcription_history.json: ");
    }));
    fv::sql::Connection staged = fv::sql::Connection::open(result.stage / "data" / "fiorivoce.db",
                                                           fv::sql::OpenMode::ReadOnly);
    CHECK(std::get<std::int64_t>(staged.scalar("SELECT COUNT(*) FROM transcription_history")) == 0);
}
