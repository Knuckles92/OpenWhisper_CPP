// Saved key, then the environment, then .env. Ported from OpenWhisper's
// tests/test_credentials.py (TestResolutionOrder). OpenWhisper's tests use the process-wide store
// and os.environ; here each test passes its own store, environment and .env.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "base/identity.h"
#include "base/testing/map_env_reader.h"
#include "credentials/dotenv.h"
#include "credentials/resolve.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "credentials/validate.h"
#include "test_support.h"

using fv::CredentialSource;
using fv::CredentialStore;
using fv::DotEnv;
using fv::MapEnvReader;
using fv::credentials_test::TempDir;
using fv::credentials_test::write_bytes;

namespace {

constexpr const char* kName = "FIORIVOCE_TEST_KEY";
constexpr const char* kSecret = "sk-test-secret-value-1234abcd";

// The autouse fixture: an in-memory store, an empty environment and a .env in a scratch folder
// that does not exist yet.
struct Fixture {
    CredentialStore store{std::make_shared<fv::MemoryCredentialBackend>(), fv::identity()};
    MapEnvReader env;
    TempDir temp;
    std::filesystem::path env_file = temp.path() / ".env";
    DotEnv dotenv{env_file, env};
};

} // namespace

TEST_CASE("saved key wins over environment", "[test_credentials]") {
    // from tests/test_credentials.py::TestResolutionOrder::test_saved_key_wins_over_environment
    // (its lookup_env_value check belongs to text_llm, M4)
    Fixture f;
    f.env.set(kName, "env-value");
    f.store.set(kName, kSecret);
    const auto resolved = fv::resolve_credential(f.store, f.env, f.dotenv, kName);
    CHECK(resolved.value == kSecret);
    CHECK(resolved.source == CredentialSource::Store);
    CHECK(fv::credential_source(f.store, f.env, f.dotenv, kName) == CredentialSource::Store);
    CHECK(fv::environment_shadowed(f.store, f.env, f.dotenv, kName));
}

TEST_CASE("environment is used when nothing is saved", "[test_credentials]") {
    // from
    // tests/test_credentials.py::TestResolutionOrder::test_environment_is_used_when_nothing_is_saved
    Fixture f;
    f.env.set(kName, "env-value");
    const auto resolved = fv::resolve_credential(f.store, f.env, f.dotenv, kName);
    CHECK(resolved.value == "env-value");
    CHECK(resolved.source == CredentialSource::Environment);
    CHECK(fv::credential_source(f.store, f.env, f.dotenv, kName) == CredentialSource::Environment);
    CHECK_FALSE(fv::environment_shadowed(f.store, f.env, f.dotenv, kName));
}

TEST_CASE("dotenv is the last resort", "[test_credentials]") {
    // from tests/test_credentials.py::TestResolutionOrder::test_dotenv_is_the_last_resort
    Fixture f;
    write_bytes(f.env_file, std::string(kName) + "=file-value\n");
    CHECK(fv::credential_source(f.store, f.env, f.dotenv, kName) == CredentialSource::DotEnv);
    const auto resolved = fv::resolve_credential(f.store, f.env, f.dotenv, kName);
    CHECK(resolved.value == "file-value");
    CHECK(resolved.source == CredentialSource::DotEnv);
    // OpenWhisper then has it in os.environ too, and still reports the file: an environment
    // variable holding the file's value counts as the file's.
    f.env.set(kName, "file-value");
    CHECK(fv::credential_source(f.store, f.env, f.dotenv, kName) == CredentialSource::DotEnv);
    CHECK(fv::resolve_credential(f.store, f.env, f.dotenv, kName).source ==
          CredentialSource::DotEnv);
}

TEST_CASE("dotenv edit is seen despite the parse cache", "[test_credentials]") {
    // from
    // tests/test_credentials.py::TestResolutionOrder::test_dotenv_edit_is_seen_despite_the_parse_cache
    Fixture f;
    write_bytes(f.env_file, std::string(kName) + "=first\n");
    CHECK(f.dotenv.get(kName) == "first");
    write_bytes(f.env_file, std::string(kName) + "=second-value\n");
    // A distinct time even on file systems with coarse timestamps.
    const auto time = std::filesystem::last_write_time(f.env_file);
    std::filesystem::last_write_time(f.env_file, time + std::chrono::milliseconds(1));
    CHECK(f.dotenv.get(kName) == "second-value");
    std::filesystem::remove(f.env_file);
    CHECK(f.dotenv.values().empty());
}

TEST_CASE("nothing set", "[test_credentials]") {
    // from tests/test_credentials.py::TestResolutionOrder::test_nothing_set
    Fixture f;
    const auto resolved = fv::resolve_credential(f.store, f.env, f.dotenv, kName);
    CHECK_FALSE(resolved.value.has_value());
    CHECK(resolved.source == CredentialSource::None);
    CHECK(fv::credential_source(f.store, f.env, f.dotenv, kName) == CredentialSource::None);
    CHECK_FALSE(fv::resolve_credential(f.store, f.env, f.dotenv, "").value.has_value());
    CHECK_FALSE(fv::keyring_did_not_respond(f.store.status()));
    CHECK_FALSE(fv::kUnresponsiveKeyringStatus.empty());
}

TEST_CASE("a cached parse is reused while the file is unchanged", "[test_credentials]") {
    // credentials.py:497-522: the parse is keyed on path, mtime and size, so an edit that keeps
    // both is not seen (the cache exists because Settings asks on every redraw).
    Fixture f;
    write_bytes(f.env_file, std::string(kName) + "=aaaa\n");
    const auto time = std::filesystem::last_write_time(f.env_file);
    CHECK(f.dotenv.get(kName) == "aaaa");
    write_bytes(f.env_file, std::string(kName) + "=bbbb\n");
    std::filesystem::last_write_time(f.env_file, time);
    CHECK(f.dotenv.get(kName) == "aaaa");
}

TEST_CASE("empty values count as unset at every layer", "[test_credentials]") {
    Fixture f;
    write_bytes(f.env_file, std::string(kName) + "=\n");
    CHECK_FALSE(fv::resolve_credential(f.store, f.env, f.dotenv, kName).value.has_value());
    CHECK(fv::credential_source(f.store, f.env, f.dotenv, kName) == CredentialSource::None);
    CHECK_FALSE(fv::environment_shadowed(f.store, f.env, f.dotenv, kName));
    f.store.set(kName, kSecret);
    CHECK_FALSE(fv::environment_shadowed(f.store, f.env, f.dotenv, kName));
    write_bytes(f.env_file, std::string(kName) + "=from-file\n");
    std::filesystem::last_write_time(f.env_file, std::filesystem::last_write_time(f.env_file) +
                                                     std::chrono::milliseconds(1));
    CHECK(fv::environment_shadowed(f.store, f.env, f.dotenv, kName));
}

TEST_CASE("a variable set to the empty string stops the lookup before .env", "[test_credentials]") {
    // os.getenv returns "" (falsy), and loading .env never overwrites a variable that is set.
    Fixture f;
    f.env.set(kName, "");
    write_bytes(f.env_file, std::string(kName) + "=file-value\n");
    const auto resolved = fv::resolve_credential(f.store, f.env, f.dotenv, kName);
    CHECK_FALSE(resolved.value.has_value());
    CHECK(resolved.source == CredentialSource::None);
    CHECK(fv::credential_source(f.store, f.env, f.dotenv, kName) == CredentialSource::DotEnv);
}

TEST_CASE("sources keep OpenWhisper's names", "[test_credentials]") {
    CHECK(fv::credential_source_name(CredentialSource::Store) == "stored");
    CHECK(fv::credential_source_name(CredentialSource::Environment) == "environment");
    CHECK(fv::credential_source_name(CredentialSource::DotEnv) == "dotenv");
    CHECK(fv::credential_source_name(CredentialSource::None) == "none");
}
