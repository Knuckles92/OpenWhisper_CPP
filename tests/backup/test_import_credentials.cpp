// The API keys an OpenWhisper import copies (backup/import_credentials.h). OpenWhisper_CPP only.
// The [platform] case writes to the real Credential Manager, only under service names of its own
// ("OpenWhisper_CPPTest-legacy.<pid>" for OpenWhisper's, "OpenWhisper_CPPTest.<pid>" for
// OpenWhisper_CPP's), and deletes every target it made.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "backup/import.h"
#include "backup/import_credentials.h"
#include "backup/testing/sample_data.h"
#include "base/identity.h"
#include "base/log.h"
#include "credentials/platform_backend.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "pyport/json.h"
#include "testkit/testkit.h"

namespace fs = std::filesystem;
using fv::Json;
using fv::backup::copy_credentials;
using fv::backup::CopyReport;
using fv::backup::imported_credential_name;
using fv::backup::legacy_credential_names;

namespace {

Json fixture_settings() {
    return fv::py::loads(fv::backup::testing::read_bytes(fv::testkit::source_root() / "tests" /
                                                         "fixtures" / "import" / "openwhisper" /
                                                         "openwhisper_settings.json"));
}

// An identity like `base` with its own credential service.
struct ServiceIdentity {
    ServiceIdentity(const fv::AppIdentity& base, std::string service_name)
        : service(std::move(service_name)), identity(base) {
        identity.credential_service = service;
    }
    std::string service;
    fv::AppIdentity identity;
};

// Every log record while it lives, at every level.
class LogCapture {
public:
    LogCapture() : sink_(std::make_shared<Sink>()), previous_(fv::log::level()) {
        fv::log::set_level(fv::log::Level::Debug);
        fv::log::add_sink(sink_);
    }
    ~LogCapture() {
        fv::log::remove_sink(sink_);
        fv::log::set_level(previous_);
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    std::string text() const {
        std::lock_guard lock(sink_->mutex);
        return sink_->text;
    }

private:
    struct Sink final : fv::log::Sink {
        void write(const fv::log::Record& record) override {
            std::lock_guard lock(mutex);
            text += std::string(record.logger) + " " + std::string(record.message) + "\n";
        }
        std::mutex mutex;
        std::string text;
    };
    std::shared_ptr<Sink> sink_;
    fv::log::Level previous_;
};

const std::vector<std::string> kFixtureNames = {
    "OPENAI_API_KEY",
    "OPENROUTER_API_KEY",
    "GROQ_API_KEY",
    "OPENCODE_GO_API_KEY",
    "OPENCODE_ZEN_API_KEY",
    "LMSTUDIO_API_KEY",
    "TYPESAFE_API_KEY",
    "OPENWHISPER_REMOTE_ENGINE_TOKEN",
    "OPENWHISPER_MCP_TOKEN",
    "OPENWHISPER_API_TOKEN",
    "OPENWHISPER_TOKEN",
    "OPENWHISPER_ACTION_SECRET_a1b2c3d4e5f6",
    "OPENWHISPER_WEBHOOK_SECRET_0f1e2d3c4b5a",
    "MY_LLM_KEY",
};

} // namespace

TEST_CASE("the names OpenWhisper reads keys under come from its code and its settings",
          "[import]") {
    CHECK(legacy_credential_names(fixture_settings()) == kFixtureNames);
    // Without settings, only the fixed names; rows without a usable id or variable add nothing.
    const auto fixed = std::vector<std::string>(kFixtureNames.begin(), kFixtureNames.begin() + 11);
    CHECK(legacy_credential_names(Json::object()) == fixed);
    CHECK(legacy_credential_names(Json::parse(R"({
        "actions": [{"name": "no id"}, {"id": 7}, {"id": "  "}, "row"],
        "automation_webhooks": {"id": "not a list"},
        "text_llm_profiles": [{"api_key_env": "lower_case"}, {"api_key_env": " "},
                              {"api_key_env": "OPENAI_API_KEY"}]
    })")) == fixed);
}

TEST_CASE("OpenWhisper's prefix becomes OpenWhisper_CPP's", "[import]") {
    CHECK(imported_credential_name("OPENWHISPER_MCP_TOKEN") == "OPENWHISPER_CPP_MCP_TOKEN");
    CHECK(imported_credential_name("OPENWHISPER_WEBHOOK_SECRET_0f1e") ==
          "OPENWHISPER_CPP_WEBHOOK_SECRET_0f1e");
    CHECK(imported_credential_name("OPENAI_API_KEY") == "OPENAI_API_KEY");
    CHECK(imported_credential_name("MY_OPENWHISPER_KEY") == "MY_OPENWHISPER_KEY");
}

TEST_CASE("saved keys are copied and renamed; missing names are skipped", "[import]") {
    auto backend = std::make_shared<fv::MemoryCredentialBackend>();
    const ServiceIdentity legacy(fv::legacy_identity(), "LegacyService");
    const ServiceIdentity own(fv::identity(), "OwnService");
    fv::CredentialStore from(backend, legacy.identity);
    fv::CredentialStore to(backend, own.identity);
    from.set("OPENAI_API_KEY", "sk-openai-0123456789");
    from.set("OPENWHISPER_MCP_TOKEN", "mcp-token-0123456789");
    from.set("OPENWHISPER_WEBHOOK_SECRET_0f1e2d3c4b5a", "hook-secret-0123");
    from.set("MY_LLM_KEY", "llm-key-0123456789");
    from.set("UNRELATED", "never-copied-0123");
    to.set("GROQ_API_KEY", "openwhisper-cpp-own-groq");
    to.set("OPENAI_API_KEY", "replaced-by-import");

    const CopyReport report = copy_credentials(from, to, fixture_settings());

    CHECK(report.copied == std::vector<std::string>{"OPENAI_API_KEY", "OPENWHISPER_CPP_MCP_TOKEN",
                                                    "OPENWHISPER_CPP_WEBHOOK_SECRET_0f1e2d3c4b5a",
                                                    "MY_LLM_KEY"});
    CHECK(report.skipped.size() == kFixtureNames.size() - 4);
    CHECK(report.failed.empty());
    CHECK(to.get("OPENAI_API_KEY") == "sk-openai-0123456789");
    CHECK(to.get("OPENWHISPER_CPP_MCP_TOKEN") == "mcp-token-0123456789");
    CHECK(to.get("OPENWHISPER_CPP_WEBHOOK_SECRET_0f1e2d3c4b5a") == "hook-secret-0123");
    CHECK(to.get("MY_LLM_KEY") == "llm-key-0123456789");
    CHECK(to.get("GROQ_API_KEY") == "openwhisper-cpp-own-groq"); // not in OpenWhisper's store: kept
    CHECK_FALSE(to.get("UNRELATED").has_value());
    CHECK_FALSE(to.get("OPENWHISPER_MCP_TOKEN").has_value());
    CHECK(from.get("OPENAI_API_KEY") == "sk-openai-0123456789"); // the source is only read
}

TEST_CASE("a key the store refuses is reported by name only", "[import]") {
    auto backend = std::make_shared<fv::MemoryCredentialBackend>();
    const ServiceIdentity legacy(fv::legacy_identity(), "LegacyService");
    const ServiceIdentity own(fv::identity(), "OwnService");
    fv::CredentialStore from(backend, legacy.identity);
    fv::CredentialStore to(backend, own.identity);
    // Written past the store's validation, as a hand-edited Credential Manager entry would be.
    backend->set("LegacyService", "OPENAI_API_KEY", "has a space secret-value");
    LogCapture log;
    const CopyReport report = copy_credentials(from, to, Json::object());
    CHECK(report.copied.empty());
    CHECK(report.failed == std::vector<std::string>{"OPENAI_API_KEY"});
    CHECK(log.text().find("secret-value") == std::string::npos);
}

TEST_CASE("an import copies the keys once its stage is published", "[import]") {
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "OpenWhisper";
    fs::create_directories(source);
    fs::copy_file(fv::testkit::source_root() / "tests" / "fixtures" / "import" / "openwhisper" /
                      "openwhisper_settings.json",
                  source / "openwhisper_settings.json");
    auto backend = std::make_shared<fv::MemoryCredentialBackend>();
    const ServiceIdentity legacy(fv::legacy_identity(), "LegacyService");
    const ServiceIdentity own(fv::identity(), "OwnService");
    fv::CredentialStore from(backend, legacy.identity);
    fv::CredentialStore to(backend, own.identity);
    from.set("OPENWHISPER_API_TOKEN", "api-token-0123456789");

    fv::backup::ImportOptions options;
    options.credentials_from = &from;
    options.credentials_to = &to;
    SECTION("only when asked") {
        const auto result =
            fv::backup::stage_from_data_root(fv::legacy_identity(), source, fv::identity(),
                                             temp.path() / "OpenWhisper_CPP", options);
        CHECK(result.copied_keys.empty());
        CHECK_FALSE(to.get("OPENWHISPER_CPP_API_TOKEN").has_value());
    }
    SECTION("copied") {
        options.copy_credentials = true;
        const auto result =
            fv::backup::stage_from_data_root(fv::legacy_identity(), source, fv::identity(),
                                             temp.path() / "OpenWhisper_CPP", options);
        CHECK(result.copied_keys == std::vector<std::string>{"OPENWHISPER_CPP_API_TOKEN"});
        CHECK(to.get("OPENWHISPER_CPP_API_TOKEN") == "api-token-0123456789");
    }
    SECTION("not when staging fails") {
        options.copy_credentials = true;
        fs::create_directories(temp.path() / "OpenWhisper_CPP" / ".openwhisper-cpp-restore");
        fv::backup::testing::write_bytes(
            temp.path() / "OpenWhisper_CPP" / ".openwhisper-cpp-restore" / "pending.json", "{}");
        CHECK_THROWS(fv::backup::stage_from_data_root(fv::legacy_identity(), source, fv::identity(),
                                                      temp.path() / "OpenWhisper_CPP", options));
        CHECK_FALSE(to.get("OPENWHISPER_CPP_API_TOKEN").has_value());
    }
}

TEST_CASE("keys copy between two Credential Manager services", "[import][platform]") {
#if !defined(_WIN32)
    SKIP("Credential Manager is Windows'");
#else
    const auto pid = GetCurrentProcessId();
    const ServiceIdentity legacy(fv::legacy_identity(),
                                 std::format("OpenWhisper_CPPTest-legacy.{}", pid));
    const ServiceIdentity own(fv::identity(), std::format("OpenWhisper_CPPTest.{}", pid));
    const std::vector<std::string> written = {"OPENAI_API_KEY", "OPENWHISPER_MCP_TOKEN",
                                              "OPENWHISPER_WEBHOOK_SECRET_0f1e2d3c4b5a"};
    const std::vector<std::string> copied = {"OPENAI_API_KEY", "OPENWHISPER_CPP_MCP_TOKEN",
                                             "OPENWHISPER_CPP_WEBHOOK_SECRET_0f1e2d3c4b5a"};
    const auto backend = fv::make_platform_backend();
    // Deletes every target the case may have made, also when it fails.
    const auto clean = [&] {
        for (const auto& [service, names] :
             {std::pair{legacy.service, written}, std::pair{own.service, copied}}) {
            for (const auto& name : names) {
                for (int i = 0; i < 4 && backend->remove(service, name); ++i) {
                }
            }
        }
    };
    clean();
    struct Cleanup {
        std::function<void()> run;
        ~Cleanup() { run(); }
    } cleanup{clean};

    fv::CredentialStore from(backend, legacy.identity);
    from.set("OPENAI_API_KEY", "sk-test-value-AAAA1111");
    from.set("OPENWHISPER_MCP_TOKEN", "mcp-test-value-BBBB2222");
    from.set("OPENWHISPER_WEBHOOK_SECRET_0f1e2d3c4b5a", "hook-test-value-CCCC3333");
    LogCapture log;
    fv::CredentialStore source(backend, legacy.identity);
    fv::CredentialStore target(backend, own.identity);

    const CopyReport report = copy_credentials(source, target, fixture_settings());

    CHECK(report.copied == copied);
    CHECK(report.failed.empty());
    CHECK(report.skipped.size() == kFixtureNames.size() - 3);
    fv::CredentialStore check(backend, own.identity);
    CHECK(check.get("OPENAI_API_KEY") == "sk-test-value-AAAA1111");
    CHECK(check.get("OPENWHISPER_CPP_MCP_TOKEN") == "mcp-test-value-BBBB2222");
    CHECK(check.get("OPENWHISPER_CPP_WEBHOOK_SECRET_0f1e2d3c4b5a") == "hook-test-value-CCCC3333");
    CHECK_FALSE(check.get("GROQ_API_KEY").has_value());
    // OpenWhisper's entries are only read.
    fv::CredentialStore legacy_check(backend, legacy.identity);
    CHECK(legacy_check.get("OPENWHISPER_MCP_TOKEN") == "mcp-test-value-BBBB2222");
    const std::string logged = log.text();
    for (const char* value :
         {"sk-test-value-AAAA1111", "mcp-test-value-BBBB2222", "hook-test-value-CCCC3333"}) {
        CHECK(logged.find(value) == std::string::npos);
    }
#endif
}
