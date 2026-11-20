// OpenWhisper's .env: python-dotenv's parse, replayed from golden vectors, and the review round 4
// rules for a variable that already holds the file's value (tests/test_automation_r4b_dotenv.py).
// FioriVoce never copies .env into the process environment, so OpenWhisper's "what children
// inherit" checks become credential_source checks, and its two tests of names and values no
// environment can hold are dropped.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "base/identity.h"
#include "base/json.h"
#include "base/testing/map_env_reader.h"
#include "credentials/dotenv.h"
#include "credentials/resolve.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "golden/golden.h"
#include "test_support.h"

using fv::CredentialSource;
using fv::CredentialStore;
using fv::DotEnv;
using fv::MapEnvReader;
using fv::credentials_test::LogCapture;
using fv::credentials_test::TempDir;
using fv::credentials_test::write_bytes;

namespace {

// The `dotenv` fixture: a .env in a scratch folder and an environment of the test's own.
struct Fixture {
    CredentialStore store{std::make_shared<fv::MemoryCredentialBackend>(), fv::identity()};
    MapEnvReader env;
    TempDir temp;
    std::filesystem::path env_file = temp.path() / ".env";
    DotEnv dotenv{env_file, env};

    void write(std::string_view text) { write_bytes(env_file, text); }
    CredentialSource source(std::string_view name) {
        return fv::credential_source(store, env, dotenv, name);
    }
};

fv::Json to_json(const std::vector<std::pair<std::string, std::optional<std::string>>>& values) {
    fv::Json out = fv::Json::array();
    for (const auto& [name, value] : values) {
        out.push_back(fv::Json::array({name, value.has_value() ? fv::Json(*value) : fv::Json()}));
    }
    return out;
}

} // namespace

TEST_CASE("a variable already holding the files value came from the file",
          "[test_automation_r4b_dotenv]") {
    // from
    // tests/test_automation_r4b_dotenv.py::test_a_variable_already_holding_the_files_value_came_from_the_file
    Fixture f;
    f.env.set("OW_R4_GROQ_API_KEY", "sk-same"); // an editor started the app with .env loaded
    f.write("OW_R4_GROQ_API_KEY=sk-same\n");
    const auto resolved = fv::resolve_credential(f.store, f.env, f.dotenv, "OW_R4_GROQ_API_KEY");
    CHECK(resolved.value == "sk-same"); // the app still reads it
    CHECK(resolved.source == CredentialSource::DotEnv);
    CHECK(f.source("OW_R4_GROQ_API_KEY") == CredentialSource::DotEnv);
}

TEST_CASE("a variable holding another value is the users", "[test_automation_r4b_dotenv]") {
    // from tests/test_automation_r4b_dotenv.py::test_a_variable_holding_another_value_is_the_users
    Fixture f;
    f.env.set("OW_R4_GROQ_API_KEY", "sk-exported");
    f.write("OW_R4_GROQ_API_KEY=sk-from-dotenv\n");
    const auto resolved = fv::resolve_credential(f.store, f.env, f.dotenv, "OW_R4_GROQ_API_KEY");
    CHECK(resolved.value == "sk-exported");
    CHECK(resolved.source == CredentialSource::Environment);
    CHECK(f.source("OW_R4_GROQ_API_KEY") == CredentialSource::Environment);
}

TEST_CASE("on windows a name in dotenv matches whatever its case", "[test_automation_r4b_dotenv]") {
    // from
    // tests/test_automation_r4b_dotenv.py::test_on_windows_a_name_in_dotenv_matches_whatever_its_case
#if defined(_WIN32)
    Fixture f;
    f.env.set("OW_R4_SAME_KEY", "sk-same");
    f.env.set("OW_R4_OWN_KEY", "sk-exported");
    f.write("ow_r4_same_key=sk-same\nOw_R4_Own_Key=sk-from-dotenv\n");
    CHECK(f.source("OW_R4_SAME_KEY") == CredentialSource::DotEnv);
    CHECK(f.source("OW_R4_OWN_KEY") == CredentialSource::Environment);
    CHECK(fv::resolve_credential(f.store, f.env, f.dotenv, "OW_R4_OWN_KEY").value == "sk-exported");
    // Nothing in the environment: the file's entry answers, whatever the case of either name.
    CHECK(fv::resolve_credential(f.store, f.env, f.dotenv, "Ow_R4_Same_Key").value == "sk-same");
    CHECK(fv::resolve_credential(f.store, f.env, f.dotenv, "OW_R4_SAME_KEY").source ==
          CredentialSource::DotEnv);
#else
    SKIP("Windows ignores case in variable names");
#endif
}

TEST_CASE("golden dotenv values", "[golden]") {
    fv::golden::for_each("services.credentials", "dotenv_values", [](const fv::golden::Case& c) {
        TempDir temp;
        const auto file = temp.path() / ".env";
        write_bytes(file, c.arg<std::string>(0));
        MapEnvReader env;
        for (const auto& [name, value] : c.args()[1].items()) {
            env.set(name, value.get<std::string>());
        }
        const DotEnv dotenv(file, env);
        CHECK(fv::golden::equal(c.result<fv::Json>(), to_json(dotenv.values())));
    });
}

TEST_CASE("an unparsable statement is logged by line and skipped") {
    LogCapture log;
    MapEnvReader env;
    const auto values = fv::parse_dotenv("A=1\n\n'unterminated=secret\nB=2\n", env);
    REQUIRE(values.size() == 2);
    CHECK(values[1].first == "B");
    CHECK(log.contains("dotenv.main python-dotenv could not parse statement starting at line 2"));
    CHECK_FALSE(log.contains("secret"));
}

TEST_CASE("text parsed as it is counts \\r\\n and a lone \\r as line ends") {
    // python-dotenv over io.StringIO of this text: {"A": "1", "D": "4"}, warnings at lines 2, 6.
    LogCapture log;
    MapEnvReader env;
    const auto values = fv::parse_dotenv("A=1\r'bad\rB=2\r\n'c\nD=4\r\r'e\n", env);
    REQUIRE(values.size() == 2);
    CHECK(values[0] == std::pair<std::string, std::optional<std::string>>("A", "1"));
    CHECK(values[1] == std::pair<std::string, std::optional<std::string>>("D", "4"));
    CHECK(log.contains("could not parse statement starting at line 2\n"));
    CHECK(log.contains("could not parse statement starting at line 6\n"));
}

TEST_CASE("a .env that is not UTF-8 reads as empty, and the warning shows no content") {
    LogCapture log;
    TempDir temp;
    const auto file = temp.path() / ".env";
    write_bytes(file, "KEY=sk-secret\xff\n");
    MapEnvReader env;
    const DotEnv dotenv(file, env);
    CHECK(dotenv.values().empty());
    CHECK(log.contains("Failed to read .env file: 'utf-8' codec can't decode byte 0xff in "
                       "position 13: invalid start byte"));
    CHECK_FALSE(log.contains("sk-secret"));
}

TEST_CASE("a missing .env or a folder named .env reads as empty") {
    TempDir temp;
    MapEnvReader env;
    CHECK(DotEnv(temp.path() / ".env", env).values().empty());
    std::filesystem::create_directories(temp.path() / "folder" / ".env");
    CHECK(DotEnv(temp.path() / "folder" / ".env", env).values().empty());
}

TEST_CASE("interpolation reads the environment as Python's os.environ dict does") {
    MapEnvReader env{{"FV_DOTENV_UPPER", "up"}, {"FV_DOTENV_Mixed", "mixed"}};
    const auto values = fv::parse_dotenv(
        "A=${FV_DOTENV_UPPER}\nB=${FV_DOTENV_Mixed}\nC=${FV_DOTENV_MISSING:-d}\n", env);
    REQUIRE(values.size() == 3);
    CHECK(values[0].second == "up");
#if defined(_WIN32)
    // os.environ's keys are upper-cased on Windows, so a mixed-case reference finds nothing.
    CHECK(values[1].second == "");
#else
    CHECK(values[1].second == "mixed");
#endif
    CHECK(values[2].second == "d");
}

TEST_CASE("a name repeated in .env keeps its first place and its last value") {
    MapEnvReader env;
    const auto values = fv::parse_dotenv("A=1\nB=2\nA=3\n", env);
    REQUIRE(values.size() == 2);
    CHECK(values[0] == std::pair<std::string, std::optional<std::string>>("A", "3"));
    CHECK(values[1] == std::pair<std::string, std::optional<std::string>>("B", "2"));
}
