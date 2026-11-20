// TypeSafe's place on the API keys page and the lookups behind the Fast judgments page: the row
// every credential list ends with, and which routes have a key, over an in-memory credential
// store, an environment and a .env file of the test's own.
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <QString>

#include "app_pages_support.h"
#include "base/identity.h"
#include "testkit/testkit.h"
#include "ui_settings/api_keys_section.h"
#include "ui_settings/typesafe_credential.h"

using fv::Json;
using namespace fvtest;

namespace {

struct KeysRig {
    KeysRig()
        : store(std::make_shared<fv::MemoryCredentialBackend>(), fv::identity()),
          dotenv(root.path() / ".env", env) {}

    fv::JudgeKeys keys(fv::JudgeKeys::Verify verify = {}) {
        return fv::JudgeKeys(fv::llm::KeySources{store, env, dotenv}, std::move(verify));
    }

    fv::testkit::TempDataRoot root;
    fv::CredentialStore store;
    fv::MapEnvReader env;
    fv::DotEnv dotenv;
};

} // namespace

TEST_CASE("TypeSafe is the API keys page's last credential owner", "[widget]") {
    const fv::appcore::CredentialOwner owner = fv::typesafe_credential::owner();
    CHECK(owner.env_name == "TYPESAFE_API_KEY");
    CHECK(owner.owner == "TypeSafe");
    CHECK(owner.use == fv::appcore::kUseDecisions);
    CHECK(fv::typesafe_credential::is_typesafe("TYPESAFE_API_KEY"));
    CHECK_FALSE(fv::typesafe_credential::is_typesafe("OPENROUTER_API_KEY"));

    const auto owners = fv::credentialOwners(Json::object());
    REQUIRE_FALSE(owners.empty());
    CHECK(owners.back().env_name == "TYPESAFE_API_KEY");
    const auto entries = fv::credentialEntries(Json::object());
    REQUIRE_FALSE(entries.empty());
    CHECK(entries.back().env_name == "TYPESAFE_API_KEY");
    CHECK(entries.back().label == "TypeSafe · TYPESAFE_API_KEY");
    CHECK(fv::credentialLabel(Json::object(), "TYPESAFE_API_KEY") ==
          QStringLiteral("TypeSafe API key"));
}

TEST_CASE("a route's key is found in the store, the environment or .env", "[widget]") {
    KeysRig rig;
    fv::JudgeKeys keys = rig.keys();
    const fv::llm::JudgeRoute& typesafe = fv::llm::typesafe_route();
    const fv::llm::JudgeRoute& openrouter = fv::llm::openrouter_route();
    struct Found {
        bool before = true;
        std::optional<std::string> beforeKey;
        bool stored = false;
        std::optional<std::string> storedKey;
        bool environment = false;
    };
    // The store refuses the GUI thread, so the lookups run off it and the checks stay here.
    const Found found = offGui([&] {
        Found result;
        result.before = keys.present(typesafe);
        result.beforeKey = keys.key(typesafe);
        rig.store.set("TYPESAFE_API_KEY", "ts-live-0123456789abcdefghij");
        result.stored = keys.present(typesafe);
        result.storedKey = keys.key(typesafe);
        rig.env.set("OPENROUTER_API_KEY", "sk-or-environment-0123456789");
        result.environment = keys.present(openrouter);
        return result;
    });
    CHECK_FALSE(found.before);
    CHECK_FALSE(found.beforeKey.has_value());
    CHECK(found.stored);
    CHECK(found.storedKey == std::optional<std::string>("ts-live-0123456789abcdefghij"));
    CHECK(found.environment);
}

TEST_CASE("a route's key in .env alone counts, and a saved key comes before it", "[widget]") {
    KeysRig rig;
    {
        std::ofstream file(rig.root.path() / ".env", std::ios::binary);
        file << "TYPESAFE_API_KEY=ts-dotenv-0123456789abcdefghij\n";
    }
    fv::JudgeKeys keys = rig.keys();
    const fv::llm::JudgeRoute& typesafe = fv::llm::typesafe_route();
    const auto [fromFile, saved] = offGui([&] {
        const std::optional<std::string> found = keys.key(typesafe);
        rig.store.set("TYPESAFE_API_KEY", "ts-live-0123456789abcdefghij");
        return std::pair<std::optional<std::string>, std::optional<std::string>>(
            found, keys.key(typesafe));
    });
    CHECK(fromFile == std::optional<std::string>("ts-dotenv-0123456789abcdefghij"));
    CHECK(saved == std::optional<std::string>("ts-live-0123456789abcdefghij"));
}

TEST_CASE("an endpoint without a key variable always has the placeholder key", "[widget]") {
    KeysRig rig;
    fv::JudgeKeys keys = rig.keys();
    // A provider counts only with a model (the pair is validated together).
    const fv::llm::JudgeRoute route = fv::llm::text_model_route(
        Json{{"meeting_llm_provider", "ollama"}, {"meeting_llm_model", "qwen3:4b"}});
    REQUIRE(route.credential_env.empty());
    const auto [present, key] = offGui([&] {
        return std::pair<bool, std::optional<std::string>>(keys.present(route), keys.key(route));
    });
    CHECK(present);
    CHECK(key == std::optional<std::string>("dummy"));
}

TEST_CASE("a verification is asked over the route with the key, and never leaks a failure",
          "[widget]") {
    KeysRig rig;
    std::string route;
    std::string key;
    fv::JudgeKeys keys = rig.keys([&](const fv::llm::JudgeRoute& asked, const std::string& value) {
        route = asked.id;
        key = value;
        return fv::llm::VerifyResult{true, "accepted"};
    });
    struct Asked {
        std::optional<fv::llm::VerifyResult> withoutKey;
        std::string routeBefore;
        std::optional<fv::llm::VerifyResult> withKey;
    };
    const Asked asked = offGui([&] {
        Asked result;
        // No key to send: nothing is verified.
        result.withoutKey = keys.verifyRoute(fv::llm::openrouter_route());
        result.routeBefore = route;
        rig.store.set("OPENROUTER_API_KEY", "sk-or-v1-0123456789abcdefghij");
        result.withKey = keys.verifyRoute(fv::llm::openrouter_route());
        return result;
    });
    CHECK_FALSE(asked.withoutKey.has_value());
    CHECK(asked.routeBefore.empty());
    REQUIRE(asked.withKey.has_value());
    CHECK(*asked.withKey == fv::llm::VerifyResult{true, "accepted"});
    CHECK(route == "openrouter");
    CHECK(key == "sk-or-v1-0123456789abcdefghij");

    fv::JudgeKeys failing =
        rig.keys([](const fv::llm::JudgeRoute&, const std::string& value) -> fv::llm::VerifyResult {
            throw std::runtime_error("the server echoed " + value);
        });
    const fv::llm::VerifyResult result = failing.verify(fv::llm::typesafe_route(), "ts-secret");
    CHECK_FALSE(result.ok);
    CHECK(result.message == "Verification failed.");
    // No way to verify at all reads the same.
    CHECK(rig.keys().verify(fv::llm::typesafe_route(), "ts-secret") == result);
}
