// The credential store over a fake backend. Ported from OpenWhisper's
// tests/test_credentials.py (TestCredentialStore). Its Secret Service tests are dropped (no
// Linux backend is scheduled), and its timeout helper with them: a backend that does not answer
// reports CredentialStoreTimeout itself.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>
#include <stdexcept>
#include <string>

#include "base/identity.h"
#include "base/testing/map_env_reader.h"
#include "credentials/dotenv.h"
#include "credentials/platform_backend.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "credentials/validate.h"
#include "test_support.h"

using fv::CredentialStatus;
using fv::CredentialStore;
using fv::CredentialStoreError;
using fv::CredentialStoreTimeout;
using fv::MemoryCredentialBackend;
using fv::credentials_test::LogCapture;
using fv::credentials_test::TempDir;

namespace {

constexpr const char* kName = "OPENWHISPER_CPP_TEST_KEY";
constexpr const char* kSecret = "sk-test-secret-value-1234abcd";

std::shared_ptr<MemoryCredentialBackend> memory_backend() {
    return std::make_shared<MemoryCredentialBackend>();
}

// _failing_backend: the platform store cannot be reached at all.
class FailingBackend final : public MemoryCredentialBackend {
public:
    CredentialStatus probe() override { throw std::runtime_error("no D-Bus session"); }
};

// A store that comes up later (the user started or unlocked it).
class LateBackend final : public MemoryCredentialBackend {
public:
    explicit LateBackend(std::string name = "in-memory store")
        : MemoryCredentialBackend(std::move(name)) {}
    bool available = false;
    std::string failure = "no D-Bus session";
    CredentialStatus probe() override {
        if (!available) {
            throw std::runtime_error(failure);
        }
        return MemoryCredentialBackend::probe();
    }
};

} // namespace

TEST_CASE("round trip and delete", "[test_credentials]") {
    // from tests/test_credentials.py::TestCredentialStore::test_round_trip_and_delete
    CredentialStore store(memory_backend(), fv::identity());
    CHECK(store.status().available);
    CHECK_FALSE(store.get(kName).has_value());
    store.set(kName, std::string("  ") + kSecret + "\n");
    CHECK(store.get(kName) == kSecret);
    CHECK(store.has(kName));
    CHECK(store.remove(kName));
    CHECK_FALSE(store.get(kName).has_value());
    CHECK_FALSE(store.remove(kName));
}

TEST_CASE("unavailable backend reads none and refuses writes", "[test_credentials]") {
    // from
    // tests/test_credentials.py::TestCredentialStore::test_unavailable_backend_reads_none_and_refuses_writes
    CredentialStore store(std::make_shared<FailingBackend>(), fv::identity());
    const CredentialStatus status = store.status();
    CHECK_FALSE(status.available);
    CHECK(status.reason.find("no D-Bus session") != std::string::npos);
    CHECK_FALSE(store.get(kName).has_value());
    CHECK_THROWS_AS(store.set(kName, kSecret), CredentialStoreError);
    CHECK_THROWS_AS(store.remove(kName), CredentialStoreError);
}

TEST_CASE("refresh sees a store that appears later", "[test_credentials]") {
    // from
    // tests/test_credentials.py::TestCredentialStore::test_refresh_sees_a_store_that_appears_later
    const auto backend = std::make_shared<LateBackend>();
    CredentialStore store(backend, fv::identity());
    CHECK_FALSE(store.status().available);
    CHECK_FALSE(store.get(kName).has_value());
    backend->available = true;
    CHECK_FALSE(store.status().available);
    CHECK(store.refresh().available);
    store.set(kName, kSecret);
    CHECK(store.get(kName) == kSecret);
}

TEST_CASE("rejects values that cannot be a key", "[test_credentials]") {
    // from tests/test_credentials.py::TestCredentialStore::test_rejects_values_that_cannot_be_a_key
    const std::string raw =
        GENERATE(std::string(""), std::string("   "), std::string("sk-with space"),
                 std::string("sk-tab\tinside"), std::string("sk-“smart”"), std::string(513, 'x'));
    CAPTURE(raw);
    CHECK_THROWS_AS(fv::validate_api_key(raw), std::invalid_argument);
}

TEST_CASE("secret never reaches the log", "[test_credentials]") {
    // from tests/test_credentials.py::TestCredentialStore::test_secret_never_reaches_the_log
    LogCapture log;
    CredentialStore store(memory_backend(), fv::identity());
    store.set(kName, kSecret);
    store.get(kName);
    store.remove(kName);
    CredentialStore failing(std::make_shared<FailingBackend>(), fv::identity());
    std::string message;
    try {
        failing.set(kName, kSecret);
        FAIL("set on an unavailable store did not throw");
    } catch (const CredentialStoreError& error) {
        message = error.what();
    }
    CHECK_FALSE(log.contains(kSecret));
    CHECK(message.find(kSecret) == std::string::npos);
    CHECK(log.contains(kName));
}

TEST_CASE("secret never reaches the log when the backend fails with it", "[test_credentials]") {
    // A backend whose own messages carry the value: only the exception's type is reported.
    class LeakyBackend final : public MemoryCredentialBackend {
    public:
        void set(std::string_view, std::string_view, std::string_view value) override {
            throw std::runtime_error(std::string(value));
        }
        bool remove(std::string_view, std::string_view) override {
            throw std::runtime_error(kSecret);
        }
    };
    LogCapture log;
    CredentialStore store(std::make_shared<LeakyBackend>(), fv::identity());
    std::string saving;
    std::string removing;
    try {
        store.set(kName, kSecret);
    } catch (const CredentialStoreError& error) {
        saving = error.what();
    }
    try {
        store.remove(kName);
    } catch (const CredentialStoreError& error) {
        removing = error.what();
    }
    CHECK(saving == "Couldn't save the key to in-memory store (runtime_error).");
    CHECK(removing == "Couldn't remove the key from in-memory store (runtime_error).");
    CHECK_FALSE(log.contains(kSecret));
    CHECK(log.contains(std::string("Saving ") + kName +
                       " to the credential store failed: runtime_error"));
    CHECK(log.contains(std::string("Removing ") + kName +
                       " from the credential store failed: runtime_error"));
}

TEST_CASE("mask shows at most four characters", "[test_credentials]") {
    // from tests/test_credentials.py::TestCredentialStore::test_mask_shows_at_most_four_characters
    CHECK(fv::mask_key("") == "••••"); // None
    CHECK(fv::mask_key("short") == "••••");
    CHECK(fv::mask_key(kSecret) == "••••abcd");
}

TEST_CASE("failed read is not retried until the key is saved", "[test_credentials]") {
    // from
    // tests/test_credentials.py::TestCredentialStore::test_failed_read_is_not_retried_until_the_key_is_saved
    class PromptingBackend final : public MemoryCredentialBackend {
    public:
        PromptingBackend() : MemoryCredentialBackend("fake") {}
        int reads = 0;
        std::optional<std::string> get(std::string_view, std::string_view) override {
            ++reads;
            throw std::runtime_error("prompt dismissed");
        }
    };
    const auto backend = std::make_shared<PromptingBackend>();
    CredentialStore store(backend, fv::identity());
    CHECK_FALSE(store.get(kName).has_value());
    CHECK_FALSE(store.get(kName).has_value());
    CHECK(backend->reads == 1);
    store.set(kName, kSecret);
    CHECK(store.get(kName) == kSecret);
}

TEST_CASE("hung read gives up and marks the store unusable", "[test_credentials]") {
    // from
    // tests/test_credentials.py::TestCredentialStore::test_hung_read_gives_up_and_marks_the_store_unusable
    class SilentBackend final : public MemoryCredentialBackend {
    public:
        SilentBackend() : MemoryCredentialBackend("Secret Service") {}
        std::optional<std::string> get(std::string_view, std::string_view) override {
            throw CredentialStoreTimeout(std::string(fv::kKeyringNoResponse));
        }
    };
    CredentialStore store(std::make_shared<SilentBackend>(), fv::identity());
    CHECK_FALSE(store.get(kName).has_value());
    const CredentialStatus status = store.status();
    CHECK_FALSE(status.available);
    CHECK(status.reason == fv::kKeyringNoResponse);
    CHECK(status.backend_name == "Secret Service");
    CHECK(fv::keyring_did_not_respond(status));
    const fv::MapEnvReader env{{kName, "env-value"}};
    TempDir temp;
    const fv::DotEnv dotenv(temp.path() / ".env", env);
    CHECK(fv::resolve_credential(store, env, dotenv, kName).value == "env-value");
    // Saving now explains why it cannot.
    try {
        store.set(kName, kSecret);
        FAIL("set on an unresponsive store did not throw");
    } catch (const CredentialStoreError& error) {
        CHECK(std::string(error.what()) ==
              "Secret Service is not available: the system keyring did not respond");
    }
}

TEST_CASE("explicit access request retries a denied read without logging secrets",
          "[test_credentials]") {
    // from
    // tests/test_credentials.py::TestCredentialStore::test_explicit_access_request_retries_a_denied_read_without_logging_secrets
    class PromptingBackend final : public MemoryCredentialBackend {
    public:
        PromptingBackend() : MemoryCredentialBackend("test store") {}
        bool denied = true;
        int reads = 0;
        std::optional<std::string> get(std::string_view service, std::string_view name) override {
            ++reads;
            if (denied) {
                throw std::runtime_error(kSecret);
            }
            return MemoryCredentialBackend::get(service, name);
        }
    };
    LogCapture log;
    const auto backend = std::make_shared<PromptingBackend>();
    backend->set(fv::identity().credential_service, kName, kSecret);
    CredentialStore store(backend, fv::identity());
    CHECK_FALSE(store.get(kName).has_value());
    std::string message;
    try {
        store.request_access(kName);
        FAIL("request_access did not throw");
    } catch (const CredentialStoreError& error) {
        message = error.what();
    }
    CHECK(message == "Couldn't access test store (runtime_error).");
    CHECK(message.find(kSecret) == std::string::npos);
    CHECK_FALSE(log.contains(kSecret));
    CHECK_FALSE(store.get(kName).has_value());
    CHECK(backend->reads == 2);
    backend->denied = false;
    store.request_access(kName);
    CHECK(store.get(kName) == kSecret);
    CHECK(backend->reads == 3);
}

TEST_CASE("explicit access request reprobes an unavailable store", "[test_credentials]") {
    // from
    // tests/test_credentials.py::TestCredentialStore::test_explicit_access_request_reprobes_an_unavailable_store
    const auto backend = std::make_shared<LateBackend>("test store");
    backend->failure = "locked";
    CredentialStore store(backend, fv::identity());
    CHECK_FALSE(store.status().available);
    backend->available = true;
    store.request_access(kName);
    store.set(kName, kSecret);
    CHECK(store.get(kName) == kSecret);
}

TEST_CASE("an unavailable store's copy names the backend and the reason", "[test_credentials]") {
    // credentials.py:317-323 and 299-315: the status of a probe that raised, and its error copy.
    CredentialStore store(std::make_shared<FailingBackend>(), fv::identity());
    const CredentialStatus status = store.status();
    CHECK(status.backend_name == fv::platform_backend_name());
    CHECK(status.reason == "no D-Bus session");
    try {
        store.remove(kName);
        FAIL("remove on an unavailable store did not throw");
    } catch (const CredentialStoreError& error) {
        CHECK(std::string(error.what()) ==
              status.backend_name + " is not available: no D-Bus session");
    }
    CHECK_THROWS_AS(store.request_access(kName), CredentialStoreError);
}

TEST_CASE("keys live under the identity's service, and names are stripped", "[test_credentials]") {
    const auto backend = memory_backend();
    CredentialStore store(backend, fv::identity());
    CHECK(store.service() == fv::identity().credential_service);
    store.set(" K ", kSecret);
    CHECK(backend->get(fv::identity().credential_service, "K") == kSecret);
    CHECK(store.get("K") == kSecret);
    CHECK_THROWS_AS(store.get("  "), std::invalid_argument);
    CredentialStore legacy(backend, fv::legacy_identity());
    CHECK_FALSE(legacy.get("K").has_value());
}

TEST_CASE("an empty stored value reads as no key", "[test_credentials]") {
    // credentials.py:354: `value or None`.
    const auto backend = memory_backend();
    backend->set(fv::identity().credential_service, kName, "");
    CredentialStore store(backend, fv::identity());
    CHECK_FALSE(store.get(kName).has_value());
    CHECK_FALSE(store.has(kName));
}

TEST_CASE("refresh forgets cached lookups", "[test_credentials]") {
    const auto backend = memory_backend();
    CredentialStore store(backend, fv::identity());
    CHECK_FALSE(store.get(kName).has_value());
    backend->set(fv::identity().credential_service, kName, kSecret);
    CHECK_FALSE(store.get(kName).has_value()); // cached until refresh
    CHECK(store.refresh().available);
    CHECK(store.get(kName) == kSecret);
}

TEST_CASE("a backend that times out on save marks the store unresponsive", "[test_credentials]") {
    class SilentBackend final : public MemoryCredentialBackend {
    public:
        void set(std::string_view, std::string_view, std::string_view) override {
            throw CredentialStoreTimeout(std::string(fv::kKeyringNoResponse));
        }
    };
    CredentialStore store(std::make_shared<SilentBackend>(), fv::identity());
    try {
        store.set(kName, kSecret);
        FAIL("set did not throw");
    } catch (const CredentialStoreError& error) {
        CHECK(std::string(error.what()) ==
              "Couldn't save the key to in-memory store (CredentialStoreTimeout).");
    }
    CHECK(fv::keyring_did_not_respond(store.status()));
}
