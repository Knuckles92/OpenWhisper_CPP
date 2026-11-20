#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "automation/action_model.h"
#include "automation/control.h"
#include "automation/secrets.h"
#include "automation/signing.h"
#include "automation/testing/fake_app_control.h"
#include "base/identity.h"
#include "credentials/testing/memory_backend.h"

// services/actions/runtime.py ensure_action_secret and delete_action_secret, and
// services/automation/control.py, which OpenWhisper's tests reach only through the runtime and
// the API (M5.15, M5.16).

using namespace fv::automation;
using fv::Json;

namespace {

// A backend whose writes the store refuses, as a locked keychain does.
class RefusingBackend final : public fv::MemoryCredentialBackend {
public:
    void set(std::string_view, std::string_view, std::string_view) override {
        throw fv::CredentialStoreError("The credential store refused the change.");
    }
    bool remove(std::string_view, std::string_view) override {
        throw fv::CredentialStoreError("The credential store refused the change.");
    }
};

} // namespace

TEST_CASE("a webhook action's secret is made once and kept", "[secrets]") {
    auto backend = std::make_shared<fv::MemoryCredentialBackend>();
    fv::CredentialStore store(backend, fv::identity());
    const std::string secret = ensure_action_secret(store, "a1");
    CHECK(secret.starts_with(signing::PREFIX));
    CHECK(ensure_action_secret(store, "a1") == secret);
    CHECK(store.get(action_secret_name("a1")) == secret);
    CHECK(ensure_action_secret(store, "a2") != secret);
    // It signs.
    CHECK_NOTHROW(signing::signature(secret, "evt_1", 1, "{}"));
}

TEST_CASE("two first uses at once agree on one secret", "[secrets]") {
    auto backend = std::make_shared<fv::MemoryCredentialBackend>();
    fv::CredentialStore store(backend, fv::identity());
    std::vector<std::string> secrets(8);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < secrets.size(); ++i) {
        threads.emplace_back([&, i] { secrets[i] = ensure_action_secret(store, "same"); });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (const auto& secret : secrets) {
        CHECK(secret == secrets.front());
    }
}

TEST_CASE("a deleted action's secret is forgotten", "[secrets]") {
    auto backend = std::make_shared<fv::MemoryCredentialBackend>();
    fv::CredentialStore store(backend, fv::identity());
    const std::string first = ensure_action_secret(store, "a1");
    delete_action_secret(store, "a1");
    CHECK_FALSE(store.get(action_secret_name("a1")).has_value());
    CHECK_NOTHROW(delete_action_secret(store, "never-made"));
    CHECK(ensure_action_secret(store, "a1") != first);
}

TEST_CASE("a store that refuses is SecretUnavailable", "[secrets]") {
    auto backend = std::make_shared<RefusingBackend>();
    fv::CredentialStore store(backend, fv::identity());
    CHECK_THROWS_AS(ensure_action_secret(store, "a1"), SecretUnavailable);
    CHECK_THROWS_AS(delete_action_secret(store, "a1"), SecretUnavailable);
}

TEST_CASE("live state reads empty ids as null", "[control]") {
    LiveState state{true, false, true, "action", "", "j1", "0.1.0"};
    CHECK(state.to_dict() == Json{{"recording", true},
                                  {"busy", false},
                                  {"meeting", true},
                                  {"mode", "action"},
                                  {"action_id", nullptr},
                                  {"job_id", "j1"},
                                  {"version", "0.1.0"}});
    state.action_id = "a1";
    state.job_id = "";
    CHECK(state.to_dict()["action_id"] == "a1");
    CHECK(state.to_dict()["job_id"].is_null());
}

TEST_CASE("the app control is registered once and required with a refusal", "[control]") {
    set_app_control(nullptr);
    CHECK(app_control() == nullptr);
    try {
        require_app_control();
        FAIL("no refusal");
    } catch (const ControlError& error) {
        CHECK(error.code() == UNAVAILABLE);
        CHECK(error.message() == "OpenWhisper_CPP isn't ready for this yet.");
    }
    auto fake = std::make_shared<FakeAppControl>();
    set_app_control(fake);
    CHECK(require_app_control() == fake);
    CHECK(require_app_control()->run_action("a1", "hello", "api") == "job1");
    fake->refusal = ControlError(std::string(BUSY), "busy now");
    CHECK_THROWS_AS(require_app_control()->cancel(), ControlError);
    CHECK(fake->calls() == std::vector<FakeAppControl::Call>{{"run_action", {"a1", "hello", "api"}},
                                                             {"cancel", {}}});
    set_app_control(nullptr);
}
