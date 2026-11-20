#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "appcore/api_keys_model.h"
#include "llm/credentials.h"
#include "llm/judge.h"
#include "llm/typesafe.h"

namespace fv {

class AppContext;

// TypeSafe on the API keys page (ui_qt/dialogs/settings_api_keys.py): its credential is not a
// text-model profile's or a speech server's, so the page lists it as a row of its own, labels it,
// and tests it with a one-judgment probe instead of a models request.
namespace typesafe_credential {

// "TypeSafe API key" (_label).
inline constexpr std::string_view kLabel = "TypeSafe API key";

// The row key_entries appends last: "TypeSafe · TYPESAFE_API_KEY".
appcore::CredentialOwner owner();
// The variable is the TypeSafe credential's.
bool is_typesafe(std::string_view env_name);

} // namespace typesafe_credential

// What Settings asks of the judge routes' keys: whether a route's key resolves, the key itself,
// and a verified judgment with it. Every call reads the credential store or the network, so it
// blocks and runs on a worker, never on the GUI thread.
// Affinity: thread-safe.
class JudgeKeys {
public:
    // Verifies `key` over `route`; llm::verify_key over a JudgeEnvironment unless replaced.
    using Verify =
        std::function<llm::VerifyResult(const llm::JudgeRoute& route, const std::string& key)>;

    // `keep` holds what `keys` and `verify` refer to for as long as the keys live.
    JudgeKeys(llm::KeySources keys, Verify verify, std::shared_ptr<void> keep = {});

    // Where Python resolves keys (the saved key, the environment, .env) over `environment`'s own
    // sources, verified through llm::verify_key on `environment`, which must outlive the keys.
    static std::shared_ptr<JudgeKeys> over(llm::JudgeEnvironment& environment);
    // The same over the application's credential store, the process environment and the .env
    // file. Verification uses the context's JudgeEnvironment, or one built over its HTTP client
    // when nothing provided one.
    static std::shared_ptr<JudgeKeys> over(AppContext& context);

    // Whether `route`'s key resolves ("dummy" for an endpoint that takes none); a failing lookup
    // reads as no key, so it never reaches the UI.
    bool present(const llm::JudgeRoute& route);
    // Whether the environment or the .env file holds `route`'s key, without asking the
    // credential store (the part of the lookup that blocks): cheap enough for the GUI thread,
    // where Python's route_key reads both on every Test.
    bool presentOutsideStore(const llm::JudgeRoute& route);
    // route_key; nullopt when it cannot be resolved.
    std::optional<std::string> key(const llm::JudgeRoute& route);
    // The result of verifying `key`; a failure that is not a reported result reads as
    // "Verification failed." (what() may name the key).
    llm::VerifyResult verify(const llm::JudgeRoute& route, const std::string& key);
    // The route's own key, verified; nullopt when none resolves.
    std::optional<llm::VerifyResult> verifyRoute(const llm::JudgeRoute& route);

private:
    llm::KeySources keys_;
    Verify verify_;
    std::shared_ptr<void> keep_;
};

} // namespace fv
